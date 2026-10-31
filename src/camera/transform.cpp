#include "camera/transform.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace mapkern {

namespace {

constexpr double kDeg = kPi / 180;
const double kDefaultFov = 2 * std::atan(0.375) / kDeg;
constexpr double kMaxGroundAngle = 88 * kDeg;
constexpr double kInf = std::numeric_limits<double>::infinity();

double dot3(const std::array<double, 3>& a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

using Mat3 = std::array<std::array<double, 3>, 3>;

Mat3 multiply3(const Mat3& a, const Mat3& b) {
  Mat3 out{};
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 3; c++) out[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c];
  return out;
}

Mat3 invert3(const Mat3& m) {
  const double a = m[0][0], b = m[0][1], c = m[0][2];
  const double d = m[1][0], e = m[1][1], f = m[1][2];
  const double g = m[2][0], h = m[2][1], i = m[2][2];
  const double A = e * i - f * h;
  const double B = f * g - d * i;
  const double C = d * h - e * g;
  const double det = a * A + b * B + c * C;
  const double k = det != 0 ? 1 / det : 0;
  return {{{A * k, (c * h - b * i) * k, (b * f - c * e) * k},
           {B * k, (a * i - c * g) * k, (c * d - a * f) * k},
           {C * k, (b * g - a * h) * k, (a * e - b * d) * k}}};
}

std::array<std::array<double, 4>, 6> frustumPlanes(const mat4::Mat4& m) {
  std::array<std::array<double, 4>, 6> planes{};
  const int rows[6] = {0, 0, 1, 1, 2, 2};
  const double signs[6] = {1, -1, 1, -1, 1, -1};
  for (int k = 0; k < 6; k++) {
    const int row = rows[k];
    const double sign = signs[k];
    const double a = m[3] + sign * m[row];
    const double b = m[7] + sign * m[row + 4];
    const double c = m[11] + sign * m[row + 8];
    const double d = m[15] + sign * m[row + 12];
    double length = std::hypot(a, b, c);
    if (length == 0) length = 1;
    planes[k] = {a / length, b / length, c / length, d / length};
  }
  return planes;
}

}

Transform::Transform() : fov_(kDefaultFov) {}

double Transform::cameraToCenterDistance() {
  update();
  return cameraDistance_;
}

std::array<double, 3> Transform::cameraPosition() {
  update();
  return cameraPos_;
}

double Transform::pixelsPerMeter() const {
  return worldSize() * std::cosh(kPi * (1 - 2 * cy_)) / kEarthCircumference;
}

double Transform::centerElevationPixels() {
  update();
  return elevationPx_;
}

double Transform::far() {
  update();
  return far_;
}

double Transform::near() {
  update();
  return near_;
}

Point Transform::centerPoint() const {
  return {width / 2 + (pad_.left - pad_.right) / 2, height / 2 + (pad_.top - pad_.bottom) / 2};
}

double Transform::globeAmount() {
  update();
  return globeAmount_;
}

double Transform::globeRadius() {
  update();
  return radius_;
}

std::array<double, 3> Transform::globeCamera() {
  update();
  return globeCam_;
}

void Transform::setProjection(Projection projection) {
  if (projection == mode_) return;
  mode_ = projection;
  dirty_ = true;
  constrain();
}

void Transform::resize(double w, double h) {
  w = std::max(1.0, w);
  h = std::max(1.0, h);
  if (w == width && h == height) return;
  width = w;
  height = h;
  dirty_ = true;
  constrain();
}

void Transform::setPadding(const Padding& padding) {
  if (padding == pad_) return;
  pad_ = padding;
  dirty_ = true;
  constrain();
}

void Transform::setMaxBounds(std::optional<Rect> bounds) {
  limits_ = bounds;
  constrain();
}

void Transform::setCenter(double lng, double lat) { setCenterWorld(mercatorX(lng), mercatorY(clampLatitude(lat))); }

void Transform::setCenterWorld(double x, double y) {
  if (!std::isfinite(x) || !std::isfinite(y)) return;
  const double nx = renderWorldCopies ? x - std::floor(x) : std::clamp(x, 0.0, 1.0);
  const double ny = std::clamp(y, 0.0, 1.0);
  if (nx == cx_ && ny == cy_) return;
  cx_ = nx;
  cy_ = ny;
  dirty_ = true;
  constrain();
}

void Transform::setZoom(double zoom) {
  if (!std::isfinite(zoom)) return;
  const double z = std::clamp(zoom, minZoom, maxZoom);
  if (z == z_) return;
  z_ = z;
  dirty_ = true;
  constrain();
}

void Transform::setBearing(double bearing) {
  if (!std::isfinite(bearing)) return;
  double b = std::fmod(std::fmod(bearing, 360) + 360, 360);
  if (b > 180) b -= 360;
  if (b == bearing_) return;
  bearing_ = b;
  dirty_ = true;
  constrain();
}

double Transform::lowestZoom() {
  if (!keepInWorld || mode_ == Projection::Globe) return minZoom;
  return std::max(minZoom, std::min(maxZoom, view().fit));
}

Transform::ViewExtent Transform::view() const {
  const bool wraps = renderWorldCopies && !limits_;
  Rect bounds{limits_ ? limits_->x0 : 0, std::max(0.0, limits_ ? limits_->y0 : 0), limits_ ? limits_->x1 : 1,
              std::min(1.0, limits_ ? limits_->y1 : 1)};
  const Point cp = centerPoint();
  const double angle = bearing_ * kDeg;
  const double cosb = std::cos(angle);
  const double sinb = std::sin(angle);
  const double p = pitch_ * kDeg;
  const double tilt = std::sin(p);
  const double upright = std::cos(p);
  const double d = height / 2 / std::tan(fov_ * kDeg / 2);
  const double reach = d * upright * std::tan(kMaxGroundAngle);
  double left = kInf, right = -kInf, top = kInf, bottom = -kInf;
  const double corners[4][2] = {{-cp.x, -cp.y}, {width - cp.x, -cp.y}, {width - cp.x, height - cp.y}, {-cp.x, height - cp.y}};
  for (const auto& c : corners) {
    const double sx = c[0], sy = c[1];
    const double ahead = d * tilt - sy * upright;
    const double down = d * upright + sy * tilt;
    double t = sy < 0 ? 1 : down > 0 ? d * upright / down : kInf;
    if (ahead > 0 && t * ahead > reach) t = reach / ahead;
    const double gx = t * sx;
    const double gy = sy < 0 ? sy : d * tilt - t * ahead;
    const double dx = gx * cosb - gy * sinb;
    const double dy = gx * sinb + gy * cosb;
    left = std::min(left, dx);
    right = std::max(right, dx);
    top = std::min(top, dy);
    bottom = std::max(bottom, dy);
  }
  const double spanX = wraps ? kInf : (bounds.x1 - bounds.x0) * kTileSize;
  const double spanY = (bounds.y1 - bounds.y0) * kTileSize;
  const double fit = std::log2(std::max((right - left) / spanX, (bottom - top) / spanY));
  return {bounds, wraps, left, right, top, bottom, fit};
}

void Transform::constrain() {
  if (!keepInWorld) return;
  if (mode_ == Projection::Globe && globeness(z_) > 0) return;
  const ViewExtent v = view();
  const Rect& b = v.bounds;
  double z = z_;
  if (z < v.fit) z = std::min(maxZoom, v.fit);
  const double ws = mapkern::worldSize(z);
  double x = cx_;
  if (!v.wraps) {
    const double mid = (b.x0 + b.x1) / 2;
    x = cx_ + std::round(mid - cx_);
    const double minX = b.x0 - v.left / ws;
    const double maxX = b.x1 - v.right / ws;
    x = minX > maxX ? (minX + maxX) / 2 : std::clamp(x, minX, maxX);
  }
  const double minY = b.y0 - v.top / ws;
  const double maxY = b.y1 - v.bottom / ws;
  const double y = minY > maxY ? (minY + maxY) / 2 : std::clamp(cy_, minY, maxY);
  const double nx = renderWorldCopies ? x - std::floor(x) : std::clamp(x, 0.0, 1.0);
  if (nx == cx_ && y == cy_ && z == z_) return;
  cx_ = nx;
  cy_ = y;
  z_ = z;
  dirty_ = true;
}

void Transform::setPitch(double pitch) {
  if (!std::isfinite(pitch)) return;
  const double p = std::clamp(pitch, minPitch, maxPitch);
  if (p == pitch_) return;
  pitch_ = p;
  dirty_ = true;
  constrain();
}

void Transform::setElevation(double meters) {
  if (!std::isfinite(meters) || meters == elevation_) return;
  elevation_ = meters;
  dirty_ = true;
}

void Transform::setGround(double meters) {
  if (!std::isfinite(meters) || meters == ground_) return;
  ground_ = meters;
  dirty_ = true;
}

void Transform::setPeak(double meters) {
  const double value = std::isfinite(meters) ? meters : -kInf;
  if (value == peak_) return;
  peak_ = value;
  dirty_ = true;
}

void Transform::setElevationKeepCamera(double meters) {
  if (!std::isfinite(meters) || meters == elevation_) return;
  update();
  const double ws = worldSize();
  const double camX = cameraPos_[0] / ws;
  const double camY = cameraPos_[1] / ws;
  const double camZ = cameraPos_[2] / ws;
  const double p = pitch_ * kDeg;
  const double b = bearing_ * kDeg;
  const double fx = std::sin(p) * std::sin(b);
  const double fy = -std::sin(p) * std::cos(b);
  double x = cx_;
  double y = cy_;
  double distance = 0;
  for (int i = 0; i < 6; i++) {
    const double h = meters * std::cosh(kPi * (1 - 2 * y)) / kEarthCircumference;
    distance = (camZ - h) / std::cos(p);
    const double nx = camX + fx * distance;
    const double ny = camY + fy * distance;
    const bool settled = std::abs(ny - y) < 1e-15;
    x = nx;
    y = ny;
    if (settled) break;
  }
  if (!(distance > 0) || y < 0 || y > 1) return;
  elevation_ = meters;
  setZoom(std::log2(cameraDistance_ / distance / kTileSize));
  setCenterWorld(x, y);
  dirty_ = true;
}

void Transform::setZoomRange(double lo, double hi) {
  minZoom = std::max(kMinZoom, std::min(lo, hi));
  maxZoom = std::min(static_cast<double>(kMaxTileZoom), std::max(lo, hi));
  setZoom(z_);
}

void Transform::setFov(double degrees) {
  const double next = std::clamp(degrees, 0.1, 150.0);
  if (next == fov_) return;
  fov_ = next;
  dirty_ = true;
  setPitch(pitch_);
}

void Transform::setPitchRange(double lo, double hi) {
  minPitch = std::max(0.0, std::min(lo, hi));
  maxPitch = std::min(85.0, std::max(lo, hi));
  setPitch(pitch_);
}

void Transform::update() {
  if (!dirty_) return;
  dirty_ = false;
  const double fovRad = fov_ * kDeg;
  const double p = pitch_ * kDeg;
  const double d = height / 2 / std::tan(fovRad / 2);
  cameraDistance_ = d;
  const double ws = worldSize();
  const double ppm = ws * std::cosh(kPi * (1 - 2 * cy_)) / kEarthCircumference;
  elevationPx_ = elevation_ * ppm;
  groundPx_ = std::min(ground_ * ppm, elevationPx_);
  const double camHeight = d * std::cos(p);
  const Point cp = centerPoint();
  const double topAngle = std::min(p + std::atan(cp.y / d), kMaxGroundAngle);
  const double groundAlongTopRay = (camHeight + elevationPx_ - groundPx_) / std::cos(topAngle);
  const double farDepth = groundAlongTopRay * std::cos(fovRad / 2);
  far_ = farDepth * 1.01 + 1;
  near_ = std::max(d / 100, 0.01);
  const double bearingRad = bearing_ * kDeg;
  const double reach = d * std::sin(p);
  globeCam_ = {-std::sin(bearingRad) * reach, std::cos(bearingRad) * reach, camHeight + elevationPx_};
  forward_ = {-globeCam_[0] / d, -globeCam_[1] / d, -camHeight / d};
  globeAmount_ = mode_ == Projection::Globe ? globeness(z_) : 0;
  coverCache_.clear();
  areaCache_.clear();
  if (globeAmount_ > 0) {
    const double m = kPi * (1 - 2 * cy_);
    radius_ = ws * std::cosh(m) / (2 * kPi);
    const double sinLat = std::tanh(m);
    const double cosLat = 1 / std::cosh(m);
    const double lng = 2 * kPi * cx_ - kPi;
    const double sinLng = std::sin(lng);
    const double cosLng = std::cos(lng);
    axisC_ = {cosLat * cosLng, cosLat * sinLng, sinLat};
    axisE_ = {-sinLng, cosLng, 0};
    axisN_ = {-sinLat * cosLng, -sinLat * sinLng, cosLat};
    const double rho = radius_ / globeAmount_;
    const double reachToCenter = std::hypot(globeCam_[0], globeCam_[1], globeCam_[2] + rho);
    const double globeFar = reachToCenter * 1.01 + 1;
    far_ = globeAmount_ >= 1 ? globeFar : std::max(far_, globeFar);
  }
  projMatrix = mat4::perspective(fovRad, width / height, near_, far_);
  projMatrix[8] = -((cp.x - width / 2) * 2) / width;
  projMatrix[9] = (cp.y - height / 2) * 2 / height;
  auto& v = viewMatrix;
  v = mat4::identity();
  mat4::translate(v, 0, 0, -d);
  mat4::rotateX(v, -p);
  mat4::rotateZ(v, bearing_ * kDeg);
  mat4::scale(v, 1, -1, 1);
  mat4::translate(v, -cx_ * ws, -cy_ * ws, -elevationPx_);
  pixelMatrix = mat4::multiply(projMatrix, v);
  mat4::invert(pixelMatrixInverse, pixelMatrix);
  unitMatrix = pixelMatrix;
  mat4::scale(unitMatrix, ws, ws, 1);
  const double horizontal = d * std::sin(p);
  cameraPos_ = {cx_ * ws - std::sin(bearingRad) * horizontal, cy_ * ws + std::cos(bearingRad) * horizontal,
                camHeight + elevationPx_};
  peakPx_ = std::isfinite(peak_) ? std::max(groundPx_, std::min(peak_ * ppm, cameraPos_[2] - 1)) : groundPx_;
  if (globeAmount_ > 0) {
    globeMatrix = pixelMatrix;
    mat4::translate(globeMatrix, cx_ * ws, cy_ * ws, 0);
    mat4::invert(globeMatrixInverse, globeMatrix);
  }
}

void Transform::surfaceLocal(double x, double y, double h, double out[3]) {
  update();
  const double ws = worldSize();
  double dx = x - cx_;
  dx -= std::round(dx);
  const double fx = dx * ws;
  const double fy = (y - cy_) * ws;
  const double g = globeAmount_;
  if (g <= 0) {
    out[0] = fx;
    out[1] = fy;
    out[2] = h;
    return;
  }
  const auto u = unitFromMercator(x, y);
  const double m = kPi * (1 - 2 * y);
  const double r = radius_ + h * std::cosh(kPi * (1 - 2 * cy_)) / std::cosh(m);
  const double gx = r * dot3(axisE_, u.data());
  const double gy = -r * dot3(axisN_, u.data());
  const double gz = r * dot3(axisC_, u.data()) - radius_;
  out[0] = fx + (gx - fx) * g;
  out[1] = fy + (gy - fy) * g;
  out[2] = h + (gz - h) * g;
}

std::array<double, 3> Transform::globeDirection(double lng, double lat) {
  update();
  const double a = lat * kPi / 180;
  const double o = lng * kPi / 180;
  const double u[3] = {std::cos(a) * std::cos(o), std::cos(a) * std::sin(o), std::sin(a)};
  return {dot3(axisE_, u), -dot3(axisN_, u), dot3(axisC_, u)};
}

bool Transform::globeHidden(double lx, double ly, double lz) const {
  const double g = globeAmount_;
  if (g <= 0) return false;
  const double rho = radius_ / g;
  const double nz = lz + rho;
  const double d = lx * (globeCam_[0] - lx) + ly * (globeCam_[1] - ly) + nz * (globeCam_[2] - lz);
  return d < -1e-9 * rho * rho;
}

double Transform::projectGlobe(double x, double y, double zPixels, double out[2]) {
  double l[3];
  surfaceLocal(x, y, zPixels, l);
  const auto& m = globeMatrix;
  const double cx = m[0] * l[0] + m[4] * l[1] + m[8] * l[2] + m[12];
  const double cy = m[1] * l[0] + m[5] * l[1] + m[9] * l[2] + m[13];
  const double w = m[3] * l[0] + m[7] * l[1] + m[11] * l[2] + m[15];
  out[0] = (cx / w + 1) / 2 * width;
  out[1] = (1 - cy / w) / 2 * height;
  if (w > 0 && globeHidden(l[0], l[1], l[2])) return -w;
  return w;
}

double Transform::groundFrame(double x, double y, double zPixels, double out[6]) {
  update();
  if (globeAmount_ <= 0) {
    const auto& u = unitMatrix;
    const double w = u[3] * x + u[7] * y + u[11] * zPixels + u[15];
    if (w <= 0) return w;
    const double xn = (u[0] * x + u[4] * y + u[8] * zPixels + u[12]) / w;
    const double yn = (u[1] * x + u[5] * y + u[9] * zPixels + u[13]) / w;
    const double hw = width / 2;
    const double hh = height / 2;
    out[0] = (xn + 1) * hw;
    out[1] = (1 - yn) * hh;
    out[2] = hw * (u[0] - xn * u[3]) / w;
    out[3] = hw * (u[4] - xn * u[7]) / w;
    out[4] = -hh * (u[1] - yn * u[3]) / w;
    out[5] = -hh * (u[5] - yn * u[7]) / w;
    return w;
  }
  double p[2];
  const double w = projectGlobe(x, y, zPixels, p);
  if (w <= 0) return w;
  const double sx = p[0], sy = p[1];
  const double step = 0.5 / worldSize();
  projectGlobe(x + step, y, zPixels, p);
  const double ex = p[0], ey = p[1];
  projectGlobe(x - step, y, zPixels, p);
  out[2] = (ex - p[0]) / (2 * step);
  out[4] = (ey - p[1]) / (2 * step);
  projectGlobe(x, y + step, zPixels, p);
  const double nx = p[0], ny = p[1];
  projectGlobe(x, y - step, zPixels, p);
  out[3] = (nx - p[0]) / (2 * step);
  out[5] = (ny - p[1]) / (2 * step);
  out[0] = sx;
  out[1] = sy;
  return w;
}

std::array<double, 3> Transform::globeRay(double px, double py) {
  const double nx = px / width * 2 - 1;
  const double ny = 1 - py / height * 2;
  const auto b = mat4::transform(globeMatrixInverse, nx, ny, 1, 1);
  return {b[0] / b[3] - globeCam_[0], b[1] / b[3] - globeCam_[1], b[2] / b[3] - globeCam_[2]};
}

World Transform::localToMercator(double lx, double ly, double lz) const {
  const double up = lz + radius_;
  return mercatorFromUnit(up * axisC_[0] + lx * axisE_[0] - ly * axisN_[0], up * axisC_[1] + lx * axisE_[1] - ly * axisN_[1],
                          up * axisC_[2] + lx * axisE_[2] - ly * axisN_[2]);
}

std::optional<World> Transform::sphereHit(double px, double py, double height, bool clamp) {
  const auto dir = globeRay(px, py);
  const double r = radius_ + height;
  const double ox = globeCam_[0], oy = globeCam_[1], oz = globeCam_[2] + radius_;
  const double a = dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2];
  const double b = ox * dir[0] + oy * dir[1] + oz * dir[2];
  const double c = ox * ox + oy * oy + oz * oz - r * r;
  const double disc = b * b - a * c;
  if (disc >= 0) {
    const double t = (-b - std::sqrt(disc)) / a;
    if (t >= 0) return localToMercator(ox + dir[0] * t, oy + dir[1] * t, oz + dir[2] * t - radius_);
  }
  if (!clamp) return std::nullopt;
  const double t = std::max(0.0, -b / a);
  const double qx = ox + dir[0] * t, qy = oy + dir[1] * t, qz = oz + dir[2] * t;
  const double q = std::hypot(qx, qy, qz);
  if (q < 1e-12) return World{cx_, cy_};
  return localToMercator(qx / q * r, qy / q * r, qz / q * r - radius_);
}

std::optional<World> Transform::refineGlobe(World guess, double px, double py, double h) {
  double x = guess[0], y = guess[1];
  double p[2];
  const double step = 0.25 / worldSize();
  for (int i = 0; i < 8; i++) {
    if (projectGlobe(x, y, h, p) <= 0) return std::nullopt;
    const double ex = p[0] - px, ey = p[1] - py;
    if (std::abs(ex) + std::abs(ey) < 1e-6) return World{x, y};
    projectGlobe(x + step, y, h, p);
    const double ax = (p[0] - px - ex) / step, ay = (p[1] - py - ey) / step;
    projectGlobe(x, y + step, h, p);
    const double bx = (p[0] - px - ex) / step, by = (p[1] - py - ey) / step;
    const double det = ax * by - bx * ay;
    if (!(std::abs(det) > 1e-30)) return std::nullopt;
    x -= (by * ex - bx * ey) / det;
    y -= (ax * ey - ay * ex) / det;
  }
  if (projectGlobe(x, y, h, p) <= 0) return std::nullopt;
  if (std::abs(p[0] - px) + std::abs(p[1] - py) < 0.01) return World{x, y};
  return std::nullopt;
}

std::optional<World> Transform::unprojectGlobe(double px, double py, std::optional<double> groundZ, bool clamp) {
  const double h = groundZ.value_or(elevationPx_);
  if (globeAmount_ >= 1) return sphereHit(px, py, h, clamp);
  const auto flat = flatUnproject(px, py, h);
  if (flat) {
    if (auto refined = refineGlobe(*flat, px, py, h)) return refined;
  }
  const auto round = sphereHit(px, py, h, false);
  std::optional<World> second;
  if (round) second = refineGlobe(*round, px, py, h);
  if (second || !clamp) return second;
  return flatUnprojectClamped(px, py, h);
}

std::array<float, 9> Transform::groundMatrix(double refX, double refY, double w, double h) {
  update();
  const auto& m = pixelMatrix;
  const double ws = worldSize();
  const Mat3 a{{{m[0], m[4], m[12]}, {m[1], m[5], m[13]}, {m[3], m[7], m[15]}}};
  const Mat3 window{{{w / 2, 0, w / 2}, {0, h / 2, h / 2}, {0, 0, 1}}};
  const Mat3 world{{{ws, 0, refX * ws}, {0, ws, refY * ws}, {0, 0, 1}}};
  const Mat3 inv = invert3(multiply3(multiply3(window, a), world));
  std::array<float, 9> out{};
  for (int col = 0; col < 3; col++)
    for (int row = 0; row < 3; row++) out[col * 3 + row] = static_cast<float>(inv[row][col]);
  return out;
}

mat4::Mat4 Transform::tileMatrixDouble(int z, int x, int y, int wrap, double extent) {
  update();
  const double ws = worldSize();
  const double n = std::exp2(z);
  const double tileWorld = ws / n;
  mat4::Mat4 m = pixelMatrix;
  mat4::translate(m, (wrap * n + x) * tileWorld, y * tileWorld, 0);
  mat4::scale(m, tileWorld / extent, tileWorld / extent, 1);
  return m;
}

std::array<float, 16> Transform::tileMatrix(int z, int x, int y, int wrap, double extent) {
  return mat4::toFloat(tileMatrixDouble(z, x, y, wrap, extent));
}

ScreenPoint Transform::project(double x, double y, double zPixels) {
  update();
  if (globeAmount_ > 0) {
    double p[2];
    const double w = projectGlobe(x, y, zPixels, p);
    return {p[0], p[1], w};
  }
  const double ws = worldSize();
  const auto p = mat4::transform(pixelMatrix, x * ws, y * ws, zPixels, 1);
  const double w = p[3];
  return {(p[0] / w + 1) / 2 * width, (1 - p[1] / w) / 2 * height, w};
}

double Transform::projectTo(double out[2], double x, double y, double zPixels) {
  update();
  if (globeAmount_ > 0) return projectGlobe(x, y, zPixels, out);
  const auto& m = unitMatrix;
  const double cx = m[0] * x + m[4] * y + m[8] * zPixels + m[12];
  const double cy = m[1] * x + m[5] * y + m[9] * zPixels + m[13];
  const double w = m[3] * x + m[7] * y + m[11] * zPixels + m[15];
  out[0] = (cx / w + 1) / 2 * width;
  out[1] = (1 - cy / w) / 2 * height;
  return w;
}

Point Transform::projectLngLat(double lng, double lat) {
  const auto p = project(mercatorX(lng), mercatorY(lat));
  return {p.x, p.y};
}

void Transform::screenRay(double px, double py, std::array<double, 3>& origin, std::array<double, 3>& dir) {
  update();
  const double nx = px / width * 2 - 1;
  const double ny = 1 - py / height * 2;
  const auto a = mat4::transform(pixelMatrixInverse, nx, ny, -1, 1);
  const auto b = mat4::transform(pixelMatrixInverse, nx, ny, 1, 1);
  origin = {a[0] / a[3], a[1] / a[3], a[2] / a[3]};
  dir = {b[0] / b[3] - origin[0], b[1] / b[3] - origin[1], b[2] / b[3] - origin[2]};
}

std::optional<World> Transform::unproject(double px, double py, std::optional<double> groundZ) {
  update();
  if (globeAmount_ > 0) return unprojectGlobe(px, py, groundZ, false);
  return flatUnproject(px, py, groundZ);
}

std::optional<World> Transform::flatUnproject(double px, double py, std::optional<double> groundZ) {
  std::array<double, 3> origin, dir;
  screenRay(px, py, origin, dir);
  if (dir[2] >= -1e-12) return std::nullopt;
  const double t = (groundZ.value_or(elevationPx_) - origin[2]) / dir[2];
  if (t < 0) return std::nullopt;
  const double ws = worldSize();
  return World{(origin[0] + dir[0] * t) / ws, (origin[1] + dir[1] * t) / ws};
}

World Transform::unprojectClamped(double px, double py, std::optional<double> groundZ) {
  update();
  if (globeAmount_ > 0) return unprojectGlobe(px, py, groundZ, true).value_or(World{cx_, cy_});
  return flatUnprojectClamped(px, py, groundZ);
}

World Transform::flatUnprojectClamped(double px, double py, std::optional<double> groundZ) {
  std::array<double, 3> origin, dir;
  screenRay(px, py, origin, dir);
  const double ws = worldSize();
  const double plane = groundZ.value_or(elevationPx_);
  const double camHeight = std::max(0.0, cameraPos_[2] - plane);
  const double maxHorizontal = camHeight * std::tan(kMaxGroundAngle);
  const double hx = dir[0], hy = dir[1];
  const double hl = std::hypot(hx, hy);
  if (dir[2] < -1e-12) {
    const double t = (plane - origin[2]) / dir[2];
    const double gx = origin[0] + dir[0] * t;
    const double gy = origin[1] + dir[1] * t;
    if (std::hypot(gx - cameraPos_[0], gy - cameraPos_[1]) <= maxHorizontal) return {gx / ws, gy / ws};
  }
  if (hl < 1e-12) return {cx_, cy_};
  return {(cameraPos_[0] + hx / hl * maxHorizontal) / ws, (cameraPos_[1] + hy / hl * maxHorizontal) / ws};
}

std::optional<LngLat> Transform::unprojectLngLat(double px, double py) {
  const auto w = unproject(px, py);
  if (!w) return std::nullopt;
  return LngLat{lngFromMercatorX((*w)[0]), latFromMercatorY((*w)[1])};
}

double Transform::setLocationAtPoint(World world, double px, double py, std::optional<double> z, bool keepSize) {
  update();
  if (globeAmount_ > 0) return rotateGlobeTo(world, px, py, z, keepSize);
  for (int i = 0; i < 4; i++) {
    std::optional<double> gz;
    if (z) gz = *z * worldSize();
    const World now = unprojectClamped(px, py, gz);
    const double dx = world[0] - now[0];
    const double dy = world[1] - now[1];
    setCenterWorld(cx_ + dx, cy_ + dy);
    if (elevation_ == 0 || std::abs(dx) + std::abs(dy) < 1e-15) break;
  }
  return 0;
}

double Transform::rotateGlobeTo(World world, double px, double py, std::optional<double> z, bool keepSize) {
  double zoomShift = 0;
  for (int i = 0; i < 16; i++) {
    update();
    std::optional<double> gz;
    if (z) gz = *z * worldSize();
    const World hit = unprojectClamped(px, py, gz);
    const auto t = northUpAngles(world[0], world[1]);
    const auto h = northUpAngles(hit[0], hit[1]);
    const double dNorth = t[0] - h[0];
    const double dEast = t[1] - h[1];
    if (std::abs(dNorth) < 1e-13 && std::abs(dEast) < 1e-13) break;
    const double latRad = latFromMercatorY(cy_) * kPi / 180;
    const double lat = clampLatitude(latFromMercatorY(cy_) + dNorth * 180 / kPi);
    const double dx = dEast / std::max(std::cos(latRad), 1e-6) / (2 * kPi);
    const double before = std::cosh(kPi * (1 - 2 * cy_));
    const double g = globeAmount_;
    const double x = cx_, y = cy_;
    setCenterWorld(cx_ + std::clamp(dx, -0.25, 0.25), mercatorY(lat));
    if (keepSize) {
      const double after = std::cosh(kPi * (1 - 2 * cy_));
      const double zoom = z_;
      setZoom(zoom + g * std::log2(before / after));
      zoomShift += z_ - zoom;
    }
    if (cx_ == x && cy_ == y) break;
  }
  return zoomShift;
}

std::array<double, 2> Transform::northUpAngles(double x, double y) const {
  const auto u = unitFromMercator(x, y);
  const double up = dot3(axisC_, u.data());
  const double east = dot3(axisE_, u.data());
  const double north = dot3(axisN_, u.data());
  return {std::atan2(north, up), std::atan2(east, std::hypot(north, up))};
}

void Transform::zoomAround(double zoom, double px, double py, std::optional<double> z) {
  std::optional<double> gz;
  if (z) gz = *z * worldSize();
  const auto anchor = unproject(px, py, gz);
  setZoom(zoom);
  if (anchor) setLocationAtPoint(*anchor, px, py, z);
}

std::vector<World> Transform::footprint() {
  update();
  if (globeAmount_ > 0) return globeFootprint();
  const double w = width, h = height, g = groundPx_;
  std::vector<World> pts{unprojectClamped(0, 0, g), unprojectClamped(w / 2, 0, g), unprojectClamped(w, 0, g),
                         unprojectClamped(w, h, g), unprojectClamped(0, h, g)};
  const double top = peakPx_;
  if (top > g + 0.5) {
    pts.push_back(unprojectClamped(0, 0, top));
    pts.push_back(unprojectClamped(w, 0, top));
    pts.push_back(unprojectClamped(w, h, top));
    pts.push_back(unprojectClamped(0, h, top));
  }
  return convexHull(std::move(pts));
}

std::vector<World> Transform::globeFootprint() {
  const double w = width, h = height;
  const int steps = 8;
  std::vector<World> pts;
  int missed = 0;
  auto add = [&](double sx, double sy) {
    auto hit = unproject(sx, sy);
    if (!hit) missed++;
    const World p = hit ? *hit : unprojectClamped(sx, sy);
    pts.push_back({p[0] + std::round(cx_ - p[0]), p[1]});
  };
  for (int i = 0; i < steps; i++) {
    const double f = static_cast<double>(i) / steps;
    add(w * f, 0);
    add(w, h * f);
    add(w * (1 - f), h);
    add(0, h * (1 - f));
  }
  for (double y : {0.0, 1.0}) {
    double p[2];
    const double wv = projectGlobe(cx_, y, 0, p);
    if (wv > 0 && p[0] >= 0 && p[0] <= w && p[1] >= 0 && p[1] <= h) {
      pts.push_back({cx_ - 0.5, y});
      pts.push_back({cx_ + 0.5, y});
    }
  }
  if (missed == steps * 4 || (missed > 0 && globeSpan() > 0.25)) {
    return {{cx_ - 0.5, 0}, {cx_ + 0.5, 0}, {cx_ + 0.5, 1}, {cx_ - 0.5, 1}};
  }
  return convexHull(std::move(pts));
}

double Transform::globeSpan() const {
  const double distance = std::hypot(globeCam_[0], globeCam_[1], globeCam_[2] + radius_);
  return std::acos(std::min(1.0, radius_ / distance)) / kPi;
}

double Transform::tileArea(int z, int x, int y, int wrap) {
  update();
  const double n = std::exp2(z);
  const double x0 = wrap + x / n;
  const double y0 = y / n;
  if (globeAmount_ <= 0) {
    const auto a = project(x0, y0);
    const auto b = project(x0 + 1 / n, y0);
    const auto c = project(x0 + 1 / n, y0 + 1 / n);
    const auto d = project(x0, y0 + 1 / n);
    if (a.w <= 0 || b.w <= 0 || c.w <= 0 || d.w <= 0) return 0;
    const double s = a.x * b.y - b.x * a.y + (b.x * c.y - c.x * b.y) + (c.x * d.y - d.x * c.y) + (d.x * a.y - a.x * d.y);
    return std::abs(s) / 2;
  }
  const uint64_t key = (tileKey(z, x, y) << 3) | static_cast<uint64_t>(wrap & 7);
  if (auto it = areaCache_.find(key); it != areaCache_.end()) return it->second;
  const int k = z < 2 ? 16 : z < 4 ? 6 : 2;
  const int side = k + 1;
  std::vector<double> sx(side * side), sy(side * side);
  std::vector<uint8_t> ok(side * side);
  double p[2];
  for (int j = 0; j <= k; j++) {
    for (int i = 0; i <= k; i++) {
      const int idx = j * side + i;
      ok[idx] = projectGlobe(x0 + i / (k * n), y0 + j / (k * n), 0, p) > 0 ? 1 : 0;
      sx[idx] = p[0];
      sy[idx] = p[1];
    }
  }
  double area = 0;
  for (int j = 0; j < k; j++) {
    for (int i = 0; i < k; i++) {
      const int a = j * side + i, b = a + 1, c = b + side, d = a + side;
      if (!ok[a] || !ok[b] || !ok[c] || !ok[d]) continue;
      const double s = sx[a] * sy[b] - sx[b] * sy[a] + (sx[b] * sy[c] - sx[c] * sy[b]) +
                       (sx[c] * sy[d] - sx[d] * sy[c]) + (sx[d] * sy[a] - sx[a] * sy[d]);
      area += std::abs(s) / 2;
    }
  }
  areaCache_[key] = area;
  return area;
}

std::vector<CoveringTile> Transform::globeCovering(const CoverOptions& options) {
  const double g = globeAmount_;
  const double R = radius_;
  const auto& cam = globeCam_;
  const auto& fwd = forward_;
  const double d = cameraDistance_;
  const double up = cam[2] + R;
  const double gx = up * axisC_[0] + cam[0] * axisE_[0] - cam[1] * axisN_[0];
  const double gy = up * axisC_[1] + cam[0] * axisE_[1] - cam[1] * axisN_[1];
  const double gz = up * axisC_[2] + cam[0] * axisE_[2] - cam[1] * axisN_[2];
  const double camDistance = std::hypot(gx, gy, gz);
  const double camLat = std::asin(std::clamp(gz / camDistance, -1.0, 1.0));
  const double camLng = std::atan2(gy, gx);
  const double rho = R / g;
  const double curved = std::acos(std::min(1.0, rho / std::hypot(cam[0], cam[1], cam[2] + rho))) / g;
  const double flat = (std::max(0.0, cam[2]) * std::tan(kMaxGroundAngle) + std::hypot(cam[0], cam[1])) / R;
  const double reach = std::min({curved, flat, kPi / 2});
  const double horizon = g >= 1 ? R / camDistance : std::cos(reach * 1.1 + 0.002);
  const double zoomOffset = std::log2(kTileSize / options.tileSize);
  const double centerTileZoom = z_ + zoomOffset;
  const int maxZ = std::min(options.maxZoom, kMaxTileZoom);
  const int minZ = std::min(options.minZoom, maxZ);
  const double centerCos = 1 / std::cosh(kPi * (1 - 2 * cy_));
  const auto planes = frustumPlanes(globeMatrix);
  double nearest[2] = {0, 0};
  double local[3];
  double xs[9], ys[9], zs[9];
  std::vector<CoveringTile> out;
  std::vector<TileSlot> stack{{0, 0, 0, 0}};
  while (!stack.empty()) {
    const TileSlot t = stack.back();
    stack.pop_back();
    const double n = std::exp2(t.z);
    const double x0 = t.x / n, y0 = t.y / n;
    const double lat1 = std::atan(std::sinh(kPi * (1 - 2 * y0)));
    const double lat0 = std::atan(std::sinh(kPi * (1 - 2 * (y0 + 1 / n))));
    const double lng0 = 2 * kPi * x0 - kPi;
    const double lng1 = lng0 + 2 * kPi / n;
    const double best = nearestInBox(lat0, lat1, lng0, lng1, camLat, camLng, nearest);
    if (t.z >= 1 && best < horizon - 1e-6) continue;
    int k = 0;
    for (int j = 0; j <= 2; j++) {
      for (int i = 0; i <= 2; i++) {
        surfaceLocal(x0 + i / (2 * n), y0 + j / (2 * n), 0, local);
        xs[k] = local[0];
        ys[k] = local[1];
        zs[k] = local[2];
        k++;
      }
    }
    if (t.z >= 2) {
      const double bulge = (g * R * (1 - std::cos(kPi / n)) + 1) * 1.5;
      bool outside = false;
      for (const auto& p : planes) {
        bool all = true;
        for (int s = 0; s < 9; s++) {
          if (p[0] * xs[s] + p[1] * ys[s] + p[2] * zs[s] + p[3] > -bulge) {
            all = false;
            break;
          }
        }
        if (all) {
          outside = true;
          break;
        }
      }
      if (outside) continue;
    }
    double depth = kInf;
    for (int s = 0; s < 9; s++) {
      const double dz = (xs[s] - cam[0]) * fwd[0] + (ys[s] - cam[1]) * fwd[1] + (zs[s] - cam[2]) * fwd[2];
      depth = std::min(depth, dz);
    }
    const double nx = (nearest[1] + kPi) / (2 * kPi);
    const double ny = (1 - std::asinh(std::tan(nearest[0])) / kPi) / 2;
    surfaceLocal(nx, std::clamp(ny, 0.0, 1.0), 0, local);
    const double nd = (local[0] - cam[0]) * fwd[0] + (local[1] - cam[1]) * fwd[1] + (local[2] - cam[2]) * fwd[2];
    depth = std::max(near_, std::min(depth, nd));
    const double equator = lat0 <= 0 && lat1 >= 0 ? 1 : std::cos(std::min(std::abs(lat0), std::abs(lat1)));
    const double wanted = centerTileZoom + std::min(0.0, std::log2(d / depth)) + g * std::log2(equator / centerCos);
    const int target = std::clamp(static_cast<int>(options.round ? std::round(wanted) : std::floor(wanted + 1e-9)),
                                  minZ, std::max(minZ, maxZ));
    if (t.z >= target) {
      CoveringTile c;
      c.z = t.z;
      c.x = t.x;
      c.y = t.y;
      c.wrap = 0;
      c.distance = std::hypot(xs[4] - cam[0], ys[4] - cam[1], zs[4] - cam[2]);
      out.push_back(c);
      continue;
    }
    const int z = t.z + 1, x = t.x * 2, y = t.y * 2;
    stack.push_back({z, x, y, 0});
    stack.push_back({z, x + 1, y, 0});
    stack.push_back({z, x, y + 1, 0});
    stack.push_back({z, x + 1, y + 1, 0});
  }
  std::stable_sort(out.begin(), out.end(), [](const CoveringTile& a, const CoveringTile& b) { return a.distance < b.distance; });
  if (static_cast<int>(out.size()) > options.maxTiles) out.resize(options.maxTiles);
  return out;
}

std::vector<CoveringTile> Transform::coveringTiles(const CoverOptions& options) {
  update();
  if (globeAmount_ > 0) {
    char key[96];
    std::snprintf(key, sizeof key, "%g/%d/%d/%d/%d", options.tileSize, options.minZoom, options.maxZoom,
                  options.round ? 1 : 0, options.maxTiles);
    auto it = coverCache_.find(key);
    if (it == coverCache_.end()) it = coverCache_.emplace(key, globeCovering(options)).first;
    return it->second;
  }
  const auto poly = footprint();
  if (poly.size() < 3) return {};
  const double ws = worldSize();
  const double zoomOffset = std::log2(kTileSize / options.tileSize);
  const double centerTileZoom = z_ + zoomOffset;
  const int maxZ = std::min(options.maxZoom, kMaxTileZoom);
  const int minZ = std::min(options.minZoom, maxZ);
  const auto& cam = cameraPos_;
  const double d = cameraDistance_;
  const double p = pitch_ * kDeg;
  const double b = bearing_ * kDeg;
  const double dirX = std::sin(p) * std::sin(b);
  const double dirY = -std::sin(p) * std::cos(b);
  const double baseDepth = (cam[2] - peakPx_) * std::cos(p);
  double minX = kInf, maxX = -kInf;
  for (const auto& q : poly) {
    minX = std::min(minX, q[0]);
    maxX = std::max(maxX, q[0]);
  }
  std::vector<TileSlot> stack;
  if (renderWorldCopies) {
    for (int wv = static_cast<int>(std::floor(minX)); wv <= static_cast<int>(std::floor(maxX - 1e-12)); wv++)
      stack.push_back({0, 0, 0, wv});
  } else {
    stack.push_back({0, 0, 0, 0});
  }
  std::vector<CoveringTile> out;
  while (!stack.empty()) {
    const TileSlot t = stack.back();
    stack.pop_back();
    const double n = std::exp2(t.z);
    const double x0 = t.wrap + t.x / n, x1 = t.wrap + (t.x + 1) / n;
    const double y0 = t.y / n, y1 = (t.y + 1) / n;
    if (!polygonIntersectsRect(poly, x0, y0, x1, y1)) continue;
    const double nearX = (dirX >= 0 ? x0 : x1) * ws - cam[0];
    const double nearY = (dirY >= 0 ? y0 : y1) * ws - cam[1];
    const double depth = std::max(near_, nearX * dirX + nearY * dirY + baseDepth);
    const double wanted = std::min(centerTileZoom, centerTileZoom + std::log2(d / depth));
    const int target = std::clamp(static_cast<int>(options.round ? std::round(wanted) : std::floor(wanted + 1e-9)),
                                  minZ, std::max(minZ, maxZ));
    if (t.z >= target) {
      CoveringTile c;
      c.z = t.z;
      c.x = t.x;
      c.y = t.y;
      c.wrap = t.wrap;
      c.distance = std::hypot((x0 + x1) / 2 * ws - cam[0], (y0 + y1) / 2 * ws - cam[1], cam[2] - groundPx_);
      out.push_back(c);
      continue;
    }
    const int z = t.z + 1, x = t.x * 2, y = t.y * 2;
    stack.push_back({z, x, y, t.wrap});
    stack.push_back({z, x + 1, y, t.wrap});
    stack.push_back({z, x, y + 1, t.wrap});
    stack.push_back({z, x + 1, y + 1, t.wrap});
  }
  std::stable_sort(out.begin(), out.end(), [](const CoveringTile& a, const CoveringTile& b) { return a.distance < b.distance; });
  if (static_cast<int>(out.size()) > options.maxTiles) out.resize(options.maxTiles);
  return out;
}

std::vector<World> convexHull(std::vector<World> pts) {
  std::sort(pts.begin(), pts.end(), [](const World& a, const World& b) { return a[0] < b[0] || (a[0] == b[0] && a[1] < b[1]); });
  if (pts.size() < 3) return pts;
  auto cross = [](const World& o, const World& a, const World& b) {
    return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
  };
  std::vector<World> lower, upper;
  for (const auto& p : pts) {
    while (lower.size() >= 2 && cross(lower[lower.size() - 2], lower.back(), p) <= 0) lower.pop_back();
    lower.push_back(p);
  }
  for (auto it = pts.rbegin(); it != pts.rend(); ++it) {
    while (upper.size() >= 2 && cross(upper[upper.size() - 2], upper.back(), *it) <= 0) upper.pop_back();
    upper.push_back(*it);
  }
  upper.pop_back();
  lower.pop_back();
  lower.insert(lower.end(), upper.begin(), upper.end());
  return lower;
}

bool polygonIntersectsRect(const std::vector<World>& poly, double x0, double y0, double x1, double y1) {
  double pminX = kInf, pmaxX = -kInf, pminY = kInf, pmaxY = -kInf;
  for (const auto& p : poly) {
    pminX = std::min(pminX, p[0]);
    pmaxX = std::max(pmaxX, p[0]);
    pminY = std::min(pminY, p[1]);
    pmaxY = std::max(pmaxY, p[1]);
  }
  if (pmaxX < x0 || pminX > x1 || pmaxY < y0 || pminY > y1) return false;
  const World corners[4] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
  for (size_t i = 0; i < poly.size(); i++) {
    const World& a = poly[i];
    const World& b = poly[(i + 1) % poly.size()];
    const double nx = b[1] - a[1];
    const double ny = a[0] - b[0];
    double pMin = kInf, pMax = -kInf;
    for (const auto& p : poly) {
      const double v = p[0] * nx + p[1] * ny;
      pMin = std::min(pMin, v);
      pMax = std::max(pMax, v);
    }
    double rMin = kInf, rMax = -kInf;
    for (const auto& c : corners) {
      const double v = c[0] * nx + c[1] * ny;
      rMin = std::min(rMin, v);
      rMax = std::max(rMax, v);
    }
    if (rMax < pMin || rMin > pMax) return false;
  }
  return true;
}

}
