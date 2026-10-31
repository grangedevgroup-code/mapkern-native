#pragma once

#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

#include "render/gpu.hpp"
#include "style/layer.hpp"
#include "worker/vector_task.hpp"

namespace mapkern {

struct PaintSlot {
  int slot = -1;
  bool composite = false;
};

class FeatureStates;

class PaintTable {
 public:
  ~PaintTable() { release(); }
  void update(const StyleLayer& layer, const FeatureTable& features, const std::vector<std::string>& names, double zoom,
              const FeatureStates* states, uint64_t stateVersion);
  PaintSlot slot(const std::string& name) const;
  const GpuTexture& texture() const { return texture_.valid() ? texture_ : Gpu::get().blankFloatTexture(); }
  int width() const { return width_; }
  int stride() const { return stride_; }
  double base() const { return evaluatedZoom_; }
  size_t bytes() const { return texture_.bytes; }
  void release() { texture_.release(); }

 private:
  void evaluate(const StyleLayer& layer, const FeatureTable& features, size_t f, double base, const FeatureStates* states, float* data);
  std::unordered_map<std::string, PaintSlot> slots_;
  std::vector<std::string> names_;
  GpuTexture texture_;
  int width_ = 1;
  int stride_ = 0;
  uint64_t revision_ = UINT64_MAX;
  uint64_t motion_ = UINT64_MAX;
  uint64_t stateVersion_ = UINT64_MAX;
  bool hasComposite_ = false;
  bool hasState_ = false;
  double evaluatedZoom_ = NAN;
};

class FeatureStates {
 public:
  const Object* get(const std::string& sourceLayer, double id) const;
  void set(const std::string& sourceLayer, double id, const Object& state);
  void remove(const std::string& sourceLayer, double id, const std::string& key);
  void clear(const std::string& sourceLayer);
  uint64_t version() const { return version_; }

 private:
  std::unordered_map<std::string, std::unordered_map<double, Object>> states_;
  uint64_t version_ = 0;
};

}
