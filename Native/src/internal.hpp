#pragma once
#include "pidan_pattern.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <vector>
static_assert(sizeof(pidan_watermark) == 56 && offsetof(pidan_watermark, watermark) == 45 &&
                  offsetof(pidan_watermark, mirror) == 48 &&
                  offsetof(pidan_watermark, method) == 52,
              "Watermark ABI changed");
namespace pidan {
// Also yields to the host event loop in asynchronous JS builds.
bool cancelled();
struct CancelScope {
  pidan_cancel_token *previous;
  explicit CancelScope(pidan_cancel_token *token);
  ~CancelScope();
};
using Bytes = std::vector<uint8_t>;
using Line = std::vector<double>;
using RGB = std::array<double, 3>;
constexpr int COLS = 120, TAIL = 112;
inline const std::array<int, 8> HEAD = {2, 1, 1, 1, 2, 2, 1, 2}, END = {2, 1, 2, 1, 1, 2, 2, 1};
inline int rnd(double n) {
  return int(std::floor(n + .5));
}
inline double sq(double n) {
  return n * n;
}
inline int clamp(int n, int a, int b) {
  return std::max(a, std::min(b, n));
}
struct Image {
  int w = 0, h = 0;
  Bytes data;
  Image() = default;
  Image(int x, int y) : w(x), h(y), data(size_t(x) * y * 4, 255) {}
};
struct Point {
  double k, x;
};
struct Edge {
  double x, strength;
};
struct Fit {
  double x = 0, cell = 0, error = 0;
};
struct Anchor : Fit {
  int offset = 0, bad = 0, row = 0;
  std::vector<Point> points;
};
struct Geometry : Fit {
  int offset = 0, row = 0;
  double y = 0, height = 0, score = 0;
};
struct Band {
  int top = 0, bottom = 0;
  double score = 0;
};
std::optional<pidan_watermark> decode(const std::array<int, COLS> &cells);
std::optional<pidan_watermark> fast(const Bytes &lum, int w, int h);
std::vector<Edge> edges(const Line &line, double mergeCell = 0);
std::optional<Fit> fitLine(const std::vector<Point> &points, double minCell = 2);
std::vector<Anchor> exactAnchors(const Line &line, const std::vector<Edge> &e, int offset,
                                 double coarse, bool yMode, int row = 0);
std::optional<std::array<int, COLS>> sample(const Image &im, const Geometry &geometry);
std::pair<int, int> bounds(const Image &im, const Geometry &geometry, int seed);
std::vector<Band> locateY(const Image &im);
std::optional<pidan_watermark> scanY(const Image &small, const Image &large, double inv, int yOff);
std::optional<pidan_watermark> scanBands(const Bytes &lum, int w, int h, const Image &large,
                                         double inv, int yOff);
Image resize(const Image &im, int x, int y, int w, int h, int ow, int oh);
} // namespace pidan
