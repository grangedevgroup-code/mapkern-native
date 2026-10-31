#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "core/value.hpp"

namespace mapkern::ui {

using Polyline = std::vector<std::array<double, 2>>;

struct Affine {
  double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
  std::array<double, 2> apply(double x, double y) const { return {a * x + c * y + e, b * x + d * y + f}; }
};

struct Shape {
  std::vector<Polyline> rings;
  std::vector<bool> closed;
};

Shape parsePath(const std::string& d);
Color rgba(int r, int g, int b, double a = 1);

class Canvas {
 public:
  Canvas(int width, int height, double scale);
  int width() const { return width_; }
  int height() const { return height_; }
  double scale() const { return scale_; }
  void clear();
  void roundRect(double x, double y, double w, double h, double r, const Color& color);
  void roundRectCorners(double x, double y, double w, double h, const std::array<double, 4>& radii, const Color& color);
  void shadow(double x, double y, double w, double h, double r, double blur, double dy, double alpha);
  void rect(double x, double y, double w, double h, const Color& color);
  void fill(const Shape& shape, const Affine& m, const Color& color);
  void stroke(const Shape& shape, const Affine& m, double width, const Color& color);
  double text(double x, double baseline, const std::string& text, double px, bool bold, const Color& color);
  static double measure(const std::string& text, double px, bool bold);
  static double ascent(double px, bool bold);
  std::vector<uint8_t> pixels() const;

 private:
  void blend(int x, int y, const Color& c, double coverage);
  int width_, height_;
  double scale_;
  std::vector<float> data_;
};

}
