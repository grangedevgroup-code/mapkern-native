#include <cmath>
#include <vector>

#include "check.hpp"
#include "terrain/dem.hpp"

using namespace mapkern;

namespace {

std::vector<uint8_t> terrarium(int size, const std::function<double(int, int)>& meters) {
  std::vector<uint8_t> px(static_cast<size_t>(size) * size * 4);
  for (int y = 0; y < size; y++)
    for (int x = 0; x < size; x++) {
      const double v = meters(x, y) + 32768;
      uint8_t* p = &px[(static_cast<size_t>(y) * size + x) * 4];
      p[0] = static_cast<uint8_t>(std::floor(v / 256));
      p[1] = static_cast<uint8_t>(std::floor(v)) % 256;
      p[2] = static_cast<uint8_t>(std::floor((v - std::floor(v)) * 256));
      p[3] = 255;
    }
  return px;
}

}

TEST("dem decodes terrarium and samples between cells") {
  const auto px = terrarium(4, [](int x, int) { return x * 100.0; });
  auto grid = decodeDem(px.data(), 4, unpackFor(Value("terrarium")));
  EXPECT(grid.has_value());
  EXPECT(grid->stride == 8);
  EXPECT_NEAR(grid->min, 0, 1e-3);
  EXPECT_NEAR(grid->max, 300, 1e-3);
  EXPECT_NEAR(*sampleElevation(*grid, 0.125, 0.5), 0, 1e-3);
  EXPECT_NEAR(*sampleElevation(*grid, 0.25, 0.5), 50, 1e-3);
  EXPECT_NEAR(*sampleElevation(*grid, 0.875, 0.5), 300, 1e-3);
}

TEST("dem mapbox encoding and missing cells") {
  std::vector<uint8_t> px(4 * 4 * 4, 0);
  for (int i = 0; i < 16; i++) {
    const int raw = (1000 + 10000) * 10;
    px[i * 4] = static_cast<uint8_t>(raw >> 16);
    px[i * 4 + 1] = static_cast<uint8_t>((raw >> 8) & 255);
    px[i * 4 + 2] = static_cast<uint8_t>(raw & 255);
    px[i * 4 + 3] = i == 5 ? 0 : 255;
  }
  auto grid = decodeDem(px.data(), 4, unpackFor(Value("mapbox")));
  EXPECT(grid.has_value());
  EXPECT(!grid->mask.empty());
  EXPECT(grid->mask[5] == 0);
  EXPECT(!sampleElevation(*grid, 1.5 / 4, 1.5 / 4).has_value());
  EXPECT_NEAR(*sampleElevation(*grid, 0.1, 0.1), 1000, 1e-2);
  EXPECT(!std::isnan(grid->elevation[(1 + kDemBorder) * grid->stride + 1 + kDemBorder]));
}

TEST("dem with no data is empty") {
  std::vector<uint8_t> px(4 * 4 * 4, 0);
  EXPECT(!decodeDem(px.data(), 4, unpackFor(Value())).has_value());
}

TEST("dem backfill copies neighbour edges and gradients see them") {
  const auto left = terrarium(4, [](int, int) { return 0.0; });
  const auto right = terrarium(4, [](int, int) { return 800.0; });
  auto a = *decodeDem(left.data(), 4, unpackFor(Value()));
  auto b = *decodeDem(right.data(), 4, unpackFor(Value()));
  auto before = gradients(a);
  EXPECT_NEAR(fromHalf(before[(2 * 6 + 5) * 2]), 0, 1e-6);
  EXPECT(backfill(a, b, 1, 0));
  EXPECT_NEAR(a.elevation[3 * a.stride + kDemBorder + 4], 800, 1e-3);
  auto after = gradients(a);
  EXPECT(fromHalf(after[(2 * 6 + 5) * 2]) > 100);
  EXPECT(!backfill(a, b, 0, 0));
}

TEST("dem frame rewrite matches a full gradient pass after backfill") {
  const auto left = terrarium(4, [](int x, int y) { return x * 30.0 + y * 7.0; });
  const auto right = terrarium(4, [](int x, int y) { return 500.0 + x * 11.0 - y * 3.0; });
  auto a = *decodeDem(left.data(), 4, unpackFor(Value()));
  auto b = *decodeDem(right.data(), 4, unpackFor(Value()));
  auto g = gradients(a);
  EXPECT(backfill(a, b, 1, 0));
  writeFrame(a, g);
  EXPECT(g == gradients(a));
}

TEST("dem spike repair replaces outliers") {
  std::vector<float> v(9, 10.0f);
  v[4] = 5000;
  EXPECT(removeSpikes(v, 3, 100) == 1);
  EXPECT_NEAR(v[4], 10, 1e-6);
}

TEST("half floats round trip common values") {
  const auto h = toHalf({0.0f, 1.0f, -2.5f, 1000.0f, 65504.0f});
  EXPECT(h[0] == 0x0000);
  EXPECT(h[1] == 0x3c00);
  EXPECT(h[2] == 0xc100);
  EXPECT(h[3] == 0x63d0);
  EXPECT(h[4] == 0x7bff);
}

TEST("half floats decode back to the value they were made from") {
  const std::vector<float> values{0.0f, 1.0f, -2.5f, 1000.0f, 65504.0f, 0.000061035156f, 0.0000000596046f, -0.333251953f};
  const auto h = toHalf(values);
  for (size_t i = 0; i < values.size(); i++) EXPECT_NEAR(fromHalf(h[i]), values[i], std::abs(values[i]) * 1e-3 + 1e-9);
  EXPECT(std::isinf(fromHalf(0x7c00)));
  EXPECT(std::isnan(fromHalf(0x7e00)));
  EXPECT(std::signbit(fromHalf(0x8000)));
}
