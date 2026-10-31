#pragma once

#include <array>
#include <cmath>

namespace mapkern::mat4 {

using Mat4 = std::array<double, 16>;
using Vec4 = std::array<double, 4>;

inline Mat4 identity() {
  Mat4 m{};
  m[0] = m[5] = m[10] = m[15] = 1;
  return m;
}

inline Mat4 multiply(const Mat4& a, const Mat4& b) {
  Mat4 out{};
  for (int i = 0; i < 4; i++) {
    const double b0 = b[i * 4], b1 = b[i * 4 + 1], b2 = b[i * 4 + 2], b3 = b[i * 4 + 3];
    for (int r = 0; r < 4; r++) out[i * 4 + r] = b0 * a[r] + b1 * a[4 + r] + b2 * a[8 + r] + b3 * a[12 + r];
  }
  return out;
}

inline void translate(Mat4& m, double x, double y, double z) {
  for (int r = 0; r < 4; r++) m[12 + r] = m[r] * x + m[4 + r] * y + m[8 + r] * z + m[12 + r];
}

inline void scale(Mat4& m, double x, double y, double z) {
  for (int r = 0; r < 4; r++) {
    m[r] *= x;
    m[4 + r] *= y;
    m[8 + r] *= z;
  }
}

inline void rotateX(Mat4& m, double rad) {
  const double s = std::sin(rad), c = std::cos(rad);
  for (int r = 0; r < 4; r++) {
    const double a1 = m[4 + r], a2 = m[8 + r];
    m[4 + r] = a1 * c + a2 * s;
    m[8 + r] = a2 * c - a1 * s;
  }
}

inline void rotateZ(Mat4& m, double rad) {
  const double s = std::sin(rad), c = std::cos(rad);
  for (int r = 0; r < 4; r++) {
    const double a0 = m[r], a1 = m[4 + r];
    m[r] = a0 * c + a1 * s;
    m[4 + r] = a1 * c - a0 * s;
  }
}

inline Mat4 perspective(double fovy, double aspect, double near, double far) {
  Mat4 out{};
  const double f = 1 / std::tan(fovy / 2);
  out[0] = f / aspect;
  out[5] = f;
  out[10] = (far + near) / (near - far);
  out[11] = -1;
  out[14] = (2 * far * near) / (near - far);
  return out;
}

inline Mat4 ortho(double left, double right, double bottom, double top, double near, double far) {
  Mat4 out{};
  out[0] = 2 / (right - left);
  out[5] = 2 / (top - bottom);
  out[10] = -2 / (far - near);
  out[12] = -(right + left) / (right - left);
  out[13] = -(top + bottom) / (top - bottom);
  out[14] = -(far + near) / (far - near);
  out[15] = 1;
  return out;
}

inline bool invert(Mat4& out, const Mat4& a) {
  const double a00 = a[0], a01 = a[1], a02 = a[2], a03 = a[3];
  const double a10 = a[4], a11 = a[5], a12 = a[6], a13 = a[7];
  const double a20 = a[8], a21 = a[9], a22 = a[10], a23 = a[11];
  const double a30 = a[12], a31 = a[13], a32 = a[14], a33 = a[15];
  const double b00 = a00 * a11 - a01 * a10, b01 = a00 * a12 - a02 * a10, b02 = a00 * a13 - a03 * a10;
  const double b03 = a01 * a12 - a02 * a11, b04 = a01 * a13 - a03 * a11, b05 = a02 * a13 - a03 * a12;
  const double b06 = a20 * a31 - a21 * a30, b07 = a20 * a32 - a22 * a30, b08 = a20 * a33 - a23 * a30;
  const double b09 = a21 * a32 - a22 * a31, b10 = a21 * a33 - a23 * a31, b11 = a22 * a33 - a23 * a32;
  double det = b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06;
  if (det == 0) return false;
  det = 1 / det;
  out[0] = (a11 * b11 - a12 * b10 + a13 * b09) * det;
  out[1] = (a02 * b10 - a01 * b11 - a03 * b09) * det;
  out[2] = (a31 * b05 - a32 * b04 + a33 * b03) * det;
  out[3] = (a22 * b04 - a21 * b05 - a23 * b03) * det;
  out[4] = (a12 * b08 - a10 * b11 - a13 * b07) * det;
  out[5] = (a00 * b11 - a02 * b08 + a03 * b07) * det;
  out[6] = (a32 * b02 - a30 * b05 - a33 * b01) * det;
  out[7] = (a20 * b05 - a22 * b02 + a23 * b01) * det;
  out[8] = (a10 * b10 - a11 * b08 + a13 * b06) * det;
  out[9] = (a01 * b08 - a00 * b10 - a03 * b06) * det;
  out[10] = (a30 * b04 - a31 * b02 + a33 * b00) * det;
  out[11] = (a21 * b02 - a20 * b04 - a23 * b00) * det;
  out[12] = (a11 * b07 - a10 * b09 - a12 * b06) * det;
  out[13] = (a00 * b09 - a01 * b07 + a02 * b06) * det;
  out[14] = (a31 * b01 - a30 * b03 - a32 * b00) * det;
  out[15] = (a20 * b03 - a21 * b01 + a22 * b00) * det;
  return true;
}

inline Vec4 transform(const Mat4& m, double x, double y, double z, double w) {
  return {m[0] * x + m[4] * y + m[8] * z + m[12] * w, m[1] * x + m[5] * y + m[9] * z + m[13] * w,
          m[2] * x + m[6] * y + m[10] * z + m[14] * w, m[3] * x + m[7] * y + m[11] * z + m[15] * w};
}

inline std::array<float, 16> toFloat(const Mat4& m) {
  std::array<float, 16> out{};
  for (int i = 0; i < 16; i++) out[i] = static_cast<float>(m[i]);
  return out;
}

}
