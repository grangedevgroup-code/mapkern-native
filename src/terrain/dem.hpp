#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "core/value.hpp"

namespace mapkern {

using Unpack = std::array<double, 4>;

constexpr int kDemBorder = 2;
constexpr double kLowestElevation = -11000;

struct DemGrid {
  int size = 0;
  int stride = 0;
  std::vector<float> elevation;
  std::vector<uint8_t> mask;
  float min = 0;
  float max = 0;
};

struct FrameStrip {
  int x = 0, y = 0, width = 0, height = 0;
  std::vector<float> data;
};

extern const std::array<std::array<int, 2>, 8> kNeighbors;

Unpack unpackFor(const Value& encoding);
std::optional<DemGrid> decodeDem(const uint8_t* rgba, int size, const Unpack& unpack, bool repair = false);
int removeSpikes(std::vector<float>& values, int size, double threshold);
std::optional<DemGrid> decodeElevations(const float* values, size_t count, int size);
bool backfill(DemGrid& target, const DemGrid& source, int dx, int dy);
std::vector<uint16_t> gradients(const DemGrid& grid);
std::vector<FrameStrip> frameStrips(const DemGrid& grid);
void writeFrame(const DemGrid& grid, std::vector<uint16_t>& out);
std::optional<double> sampleElevation(const DemGrid& grid, double u, double v);
std::vector<uint16_t> toHalf(const std::vector<float>& values);
float fromHalf(uint16_t h);
bool isRawElevation(const uint8_t* bytes, size_t size);

}
