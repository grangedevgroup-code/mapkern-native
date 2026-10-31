#include "geo/tessellate.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>

namespace mapkern {

namespace {

struct Vertex {
  uint32_t i = 0;
  double x = 0, y = 0;
  Vertex* prev = nullptr;
  Vertex* next = nullptr;
  int64_t z = 0;
  Vertex* prevZ = nullptr;
  Vertex* nextZ = nullptr;
  bool steiner = false;
};

struct Grid {
  double minX, minY, inv;
};

double sign(double v) { return v > 0 ? 1 : v < 0 ? -1 : 0; }

class Tess {
 public:
  explicit Tess(std::vector<uint32_t>& out) : out_(out) {}

  void run(const std::vector<double>& coords, const std::vector<uint32_t>& holeStarts) {
    const uint32_t vertexCount = static_cast<uint32_t>(coords.size() / 2);
    const uint32_t outerEnd = holeStarts.empty() ? vertexCount : holeStarts[0];
    Vertex* outer = ring(coords, 0, outerEnd, true);
    if (!outer || outer->next == outer->prev) return;
    if (!holeStarts.empty()) outer = attachHoles(coords, holeStarts, vertexCount, outer);
    if (vertexCount > 80) {
      double minX = INFINITY, minY = INFINITY, maxX = -INFINITY, maxY = -INFINITY;
      for (uint32_t i = 0; i < outerEnd; i++) {
        minX = std::min(minX, coords[i * 2]);
        minY = std::min(minY, coords[i * 2 + 1]);
        maxX = std::max(maxX, coords[i * 2]);
        maxY = std::max(maxY, coords[i * 2 + 1]);
      }
      const double size = std::max(maxX - minX, maxY - minY);
      Grid grid{minX, minY, size > 0 ? 32767 / size : 0};
      clip(outer, &grid, 0);
    } else {
      clip(outer, nullptr, 0);
    }
  }

 private:
  Vertex* insert(uint32_t i, double x, double y, Vertex* last) {
    Vertex& v = pool_.emplace_back();
    v.i = i;
    v.x = x;
    v.y = y;
    if (!last) {
      v.prev = &v;
      v.next = &v;
    } else {
      v.next = last->next;
      v.prev = last;
      last->next->prev = &v;
      last->next = &v;
    }
    return &v;
  }

  static void remove(Vertex* v) {
    v->next->prev = v->prev;
    v->prev->next = v->next;
    if (v->prevZ) v->prevZ->nextZ = v->nextZ;
    if (v->nextZ) v->nextZ->prevZ = v->prevZ;
  }

  static bool same(const Vertex* a, const Vertex* b) { return a->x == b->x && a->y == b->y; }

  static double cross(const Vertex* a, const Vertex* b, const Vertex* c) {
    return (b->x - a->x) * (c->y - b->y) - (b->y - a->y) * (c->x - b->x);
  }

  static bool inTriangle(double ax, double ay, double bx, double by, double cx, double cy, double px, double py) {
    return (cx - px) * (ay - py) - (ax - px) * (cy - py) >= 0 && (ax - px) * (by - py) - (bx - px) * (ay - py) >= 0 &&
           (bx - px) * (cy - py) - (cx - px) * (by - py) >= 0;
  }

  Vertex* ring(const std::vector<double>& coords, uint32_t start, uint32_t end, bool positive) {
    if (end <= start) return nullptr;
    double area = 0;
    for (uint32_t i = start, j = end - 1; i < end; j = i++)
      area += (coords[j * 2] - coords[i * 2]) * (coords[i * 2 + 1] + coords[j * 2 + 1]);
    Vertex* last = nullptr;
    if (positive == (area > 0)) {
      for (uint32_t i = start; i < end; i++) last = insert(i, coords[i * 2], coords[i * 2 + 1], last);
    } else {
      for (int64_t i = static_cast<int64_t>(end) - 1; i >= static_cast<int64_t>(start); i--)
        last = insert(static_cast<uint32_t>(i), coords[i * 2], coords[i * 2 + 1], last);
    }
    if (last && same(last, last->next)) {
      remove(last);
      last = last->next;
    }
    return last;
  }

  static Vertex* filterDegenerate(Vertex* start, Vertex* end = nullptr) {
    Vertex* stop = end ? end : start;
    Vertex* p = start;
    bool again;
    do {
      again = false;
      if (!p->steiner && (same(p, p->next) || cross(p->prev, p, p->next) == 0)) {
        remove(p);
        p = stop = p->prev;
        if (p == p->next) break;
        again = true;
      } else {
        p = p->next;
      }
    } while (again || p != stop);
    return stop;
  }

  void push(uint32_t a, uint32_t b, uint32_t c) {
    out_.push_back(a);
    out_.push_back(b);
    out_.push_back(c);
  }

  void clip(Vertex* start, const Grid* grid, int pass) {
    if (depth_ > 4096) return;
    depth_++;
    Vertex* ear = start;
    if (grid && pass == 0) index(ear, *grid);
    Vertex* stop = ear;
    while (ear->prev != ear->next) {
      Vertex* prev = ear->prev;
      Vertex* next = ear->next;
      if (grid ? isEarIndexed(ear, *grid) : isEar(ear)) {
        push(prev->i, ear->i, next->i);
        remove(ear);
        ear = next->next;
        stop = next->next;
        continue;
      }
      ear = next;
      if (ear == stop) {
        if (pass == 0) {
          clip(filterDegenerate(ear), grid, 1);
        } else if (pass == 1) {
          Vertex* cured = cureIntersections(filterDegenerate(ear));
          clip(cured, grid, 2);
        } else if (pass == 2) {
          if (!splitAndClip(ear, grid)) fan(ear);
        }
        break;
      }
    }
    depth_--;
  }

  static bool isEar(const Vertex* ear) {
    const Vertex* a = ear->prev;
    const Vertex* b = ear;
    const Vertex* c = ear->next;
    if (cross(a, b, c) <= 0) return false;
    const Vertex* p = c->next;
    while (p != a) {
      if (!(p->x == a->x && p->y == a->y) && !(p->x == b->x && p->y == b->y) && !(p->x == c->x && p->y == c->y) &&
          inTriangle(a->x, a->y, b->x, b->y, c->x, c->y, p->x, p->y) && cross(p->prev, p, p->next) <= 0)
        return false;
      p = p->next;
    }
    return true;
  }

  static int64_t morton(double x, double y, const Grid& grid) {
    uint32_t ix = static_cast<uint32_t>(static_cast<int64_t>(std::floor((x - grid.minX) * grid.inv)) & 0xffff);
    uint32_t iy = static_cast<uint32_t>(static_cast<int64_t>(std::floor((y - grid.minY) * grid.inv)) & 0xffff);
    ix = (ix | (ix << 8)) & 0x00ff00ff;
    ix = (ix | (ix << 4)) & 0x0f0f0f0f;
    ix = (ix | (ix << 2)) & 0x33333333;
    ix = (ix | (ix << 1)) & 0x55555555;
    iy = (iy | (iy << 8)) & 0x00ff00ff;
    iy = (iy | (iy << 4)) & 0x0f0f0f0f;
    iy = (iy | (iy << 2)) & 0x33333333;
    iy = (iy | (iy << 1)) & 0x55555555;
    return static_cast<int64_t>(ix | (iy << 1));
  }

  static bool isEarIndexed(const Vertex* ear, const Grid& grid) {
    const Vertex* a = ear->prev;
    const Vertex* b = ear;
    const Vertex* c = ear->next;
    if (cross(a, b, c) <= 0) return false;
    const double minX = std::min({a->x, b->x, c->x}), minY = std::min({a->y, b->y, c->y});
    const double maxX = std::max({a->x, b->x, c->x}), maxY = std::max({a->y, b->y, c->y});
    const int64_t minZ = morton(minX, minY, grid);
    const int64_t maxZ = morton(maxX, maxY, grid);
    auto blocks = [&](const Vertex* p) {
      return p != a && p != c && p != b && !(p->x == a->x && p->y == a->y) && !(p->x == b->x && p->y == b->y) &&
             !(p->x == c->x && p->y == c->y) && p->x >= minX && p->x <= maxX && p->y >= minY && p->y <= maxY &&
             inTriangle(a->x, a->y, b->x, b->y, c->x, c->y, p->x, p->y) && cross(p->prev, p, p->next) <= 0;
    };
    const Vertex* p = ear->prevZ;
    while (p && p->z >= minZ) {
      if (blocks(p)) return false;
      p = p->prevZ;
    }
    p = ear->nextZ;
    while (p && p->z <= maxZ) {
      if (blocks(p)) return false;
      p = p->nextZ;
    }
    return true;
  }

  static void index(Vertex* start, const Grid& grid) {
    std::vector<Vertex*> list;
    Vertex* p = start;
    do {
      p->z = morton(p->x, p->y, grid);
      list.push_back(p);
      p = p->next;
    } while (p != start);
    std::stable_sort(list.begin(), list.end(), [](const Vertex* u, const Vertex* v) { return u->z < v->z; });
    for (size_t i = 0; i < list.size(); i++) {
      list[i]->prevZ = i > 0 ? list[i - 1] : nullptr;
      list[i]->nextZ = i + 1 < list.size() ? list[i + 1] : nullptr;
    }
  }

  static bool segmentsCross(const Vertex* p1, const Vertex* q1, const Vertex* p2, const Vertex* q2) {
    const double o1 = sign(cross(p1, q1, p2));
    const double o2 = sign(cross(p1, q1, q2));
    const double o3 = sign(cross(p2, q2, p1));
    const double o4 = sign(cross(p2, q2, q1));
    return o1 != o2 && o3 != o4 && o1 != 0 && o2 != 0 && o3 != 0 && o4 != 0;
  }

  Vertex* cureIntersections(Vertex* start) {
    Vertex* p = start;
    int64_t guard = 0;
    do {
      Vertex* a = p->prev;
      Vertex* b = p->next->next;
      if (!same(a, b) && segmentsCross(a, p, p->next, b) && locallyInside(a, b) && locallyInside(b, a)) {
        push(a->i, p->i, b->i);
        remove(p);
        remove(p->next);
        p = start = b;
      }
      p = p->next;
      if (++guard > 10000000) break;
    } while (p != start);
    return filterDegenerate(p);
  }

  static bool locallyInside(const Vertex* a, const Vertex* b) {
    return cross(a->prev, a, a->next) > 0 ? cross(a, a->next, b) >= 0 && cross(a, a->prev, b) <= 0
                                          : cross(a, a->next, b) >= 0 || cross(a, a->prev, b) <= 0;
  }

  static bool intersectsPolygon(const Vertex* a, const Vertex* b) {
    const Vertex* p = a;
    do {
      if (p->i != a->i && p->next->i != a->i && p->i != b->i && p->next->i != b->i && segmentsCross(p, p->next, a, b)) return true;
      p = p->next;
    } while (p != a);
    return false;
  }

  static bool middleInside(const Vertex* a, const Vertex* b) {
    const Vertex* p = a;
    bool inside = false;
    const double px = (a->x + b->x) / 2, py = (a->y + b->y) / 2;
    do {
      if ((p->y > py) != (p->next->y > py) && p->next->y != p->y &&
          px < (p->next->x - p->x) * (py - p->y) / (p->next->y - p->y) + p->x)
        inside = !inside;
      p = p->next;
    } while (p != a);
    return inside;
  }

  static bool validDiagonal(const Vertex* a, const Vertex* b) {
    return a->next->i != b->i && a->prev->i != b->i && !intersectsPolygon(a, b) && locallyInside(a, b) && locallyInside(b, a) &&
           middleInside(a, b);
  }

  bool splitAndClip(Vertex* start, const Grid* grid) {
    Vertex* a = start;
    do {
      Vertex* b = a->next->next;
      while (b != a->prev) {
        if (a->i != b->i && validDiagonal(a, b)) {
          Vertex* c = split(a, b);
          a = filterDegenerate(a, a->next);
          c = filterDegenerate(c, c->next);
          clip(a, grid, 0);
          clip(c, grid, 0);
          return true;
        }
        b = b->next;
      }
      a = a->next;
    } while (a != start);
    return false;
  }

  Vertex* clone(const Vertex* v) {
    Vertex& c = pool_.emplace_back();
    c.i = v->i;
    c.x = v->x;
    c.y = v->y;
    c.z = v->z;
    c.steiner = v->steiner;
    return &c;
  }

  Vertex* split(Vertex* a, Vertex* b) {
    Vertex* a2 = clone(a);
    Vertex* b2 = clone(b);
    Vertex* an = a->next;
    Vertex* bp = b->prev;
    a->next = b;
    b->prev = a;
    a2->next = an;
    an->prev = a2;
    b2->next = a2;
    a2->prev = b2;
    bp->next = b2;
    b2->prev = bp;
    return b2;
  }

  void fan(Vertex* start) {
    Vertex* p = start->next;
    while (p->next != start) {
      push(start->i, p->i, p->next->i);
      p = p->next;
    }
  }

  Vertex* attachHoles(const std::vector<double>& coords, const std::vector<uint32_t>& holeStarts, uint32_t vertexCount, Vertex* outer) {
    std::vector<Vertex*> holes;
    for (size_t h = 0; h < holeStarts.size(); h++) {
      const uint32_t start = holeStarts[h];
      const uint32_t end = h + 1 < holeStarts.size() ? holeStarts[h + 1] : vertexCount;
      Vertex* list = ring(coords, start, end, false);
      if (!list) continue;
      if (list == list->next) list->steiner = true;
      holes.push_back(rightmost(list));
    }
    std::stable_sort(holes.begin(), holes.end(), [](const Vertex* a, const Vertex* b) { return a->x > b->x; });
    Vertex* result = outer;
    for (Vertex* hole : holes) result = bridgeHole(hole, result);
    return result;
  }

  static Vertex* rightmost(Vertex* start) {
    Vertex* p = start;
    Vertex* best = start;
    do {
      if (p->x > best->x || (p->x == best->x && p->y < best->y)) best = p;
      p = p->next;
    } while (p != start);
    return best;
  }

  Vertex* bridgeHole(Vertex* hole, Vertex* outer) {
    Vertex* target = findBridge(hole, outer);
    if (!target) return outer;
    Vertex* joined = split(target, hole);
    filterDegenerate(joined, joined->next);
    return filterDegenerate(target, target->next);
  }

  static Vertex* findBridge(Vertex* hole, Vertex* outer) {
    Vertex* p = outer;
    const double hx = hole->x, hy = hole->y;
    double qx = INFINITY;
    Vertex* m = nullptr;
    do {
      if (hy >= p->y && hy <= p->next->y && p->next->y != p->y) {
        const double x = p->x + (hy - p->y) * (p->next->x - p->x) / (p->next->y - p->y);
        if (x >= hx && x < qx) {
          qx = x;
          m = p->x > p->next->x ? p : p->next;
          if (x == hx) return hy == p->y ? p : hy == p->next->y ? p->next : m;
        }
      }
      p = p->next;
    } while (p != outer);
    if (!m) return nullptr;
    if (m->x == qx && m->y == hy) return m;
    Vertex* stop = m;
    const double mx = m->x, my = m->y;
    const bool above = my > hy;
    double bestTan = INFINITY, bestDist = INFINITY;
    p = m;
    do {
      if (p->x > hx && p->x <= mx && p != stop) {
        const bool inside = above ? inTriangle(hx, hy, qx, hy, mx, my, p->x, p->y) : inTriangle(qx, hy, hx, hy, mx, my, p->x, p->y);
        if (inside && locallyInside(p, hole)) {
          const double tan = std::abs(p->y - hy) / (p->x - hx);
          const double dist = p->x - hx;
          if (tan < bestTan || (tan == bestTan && dist < bestDist)) {
            m = p;
            bestTan = tan;
            bestDist = dist;
          }
        }
      }
      p = p->next;
    } while (p != stop);
    return m;
  }

  std::vector<uint32_t>& out_;
  std::deque<Vertex> pool_;
  int depth_ = 0;
};

}

void triangulate(const std::vector<double>& coords, const std::vector<uint32_t>& holeStarts, std::vector<uint32_t>& out) {
  if (coords.size() < 6) return;
  Tess tess(out);
  tess.run(coords, holeStarts);
}

double triangleArea(const std::vector<double>& coords, const std::vector<uint32_t>& indices) {
  double sum = 0;
  for (size_t t = 0; t + 2 < indices.size(); t += 3) {
    const size_t a = indices[t] * 2, b = indices[t + 1] * 2, c = indices[t + 2] * 2;
    sum += std::abs((coords[b] - coords[a]) * (coords[c + 1] - coords[a + 1]) - (coords[c] - coords[a]) * (coords[b + 1] - coords[a + 1])) / 2;
  }
  return sum;
}

}
