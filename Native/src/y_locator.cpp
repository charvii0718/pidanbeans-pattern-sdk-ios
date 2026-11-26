// Ported from packages/watermark-recognizer/src/watermarkYLocator.ts; TS remains the regression
// oracle.
#include "internal.hpp"
namespace pidan {
std::vector<Band> locateY(const Image &im) {
  int w = im.w, h = im.h, step = 3, rowStep = 3, n = (w + 2) / 3;
  if (w < 8 || h < 3)
    return {};
  Bytes reference(n * 3), current(n * 3), edge(n), refEdge(n), mask(n), boundaryEdges(n),
      boundaryMatches(n);
  std::vector<int> good(n);
  std::vector<Band> results;
  int top = -1, bottom = -1, left = 0, right = 0, rows = 0, seedEdges = 0;
  auto colorCoverage = [&](int y1, int y2, int x1, int x2) {
    std::array<int, 4096> counts{}, red{}, green{}, blue{};
    std::vector<int> touched;
    int total = 0;
    for (int y = y1; y <= y2; y += 2)
      for (int x = x1; x <= x2; x++) {
        size_t p = (size_t(y) * w + x * step) * 4;
        int r = im.data[p], g = im.data[p + 1], b = im.data[p + 2],
            key = ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
        if (!counts[key])
          touched.push_back(key);
        counts[key]++;
        red[key] += r;
        green[key] += g;
        blue[key] += b;
        total++;
      }
    std::set<int> used;
    int covered = 0;
    for (int group = 0; group < 2; group++) {
      int seed = -1;
      for (int k : touched)
        if (!used.count(k) && (seed < 0 || counts[k] > counts[seed]))
          seed = k;
      if (seed < 0)
        break;
      double r = double(red[seed]) / counts[seed], g = double(green[seed]) / counts[seed],
             b = double(blue[seed]) / counts[seed];
      for (int k : touched)
        if (!used.count(k) && sq(double(red[k]) / counts[k] - r) +
                                      sq(double(green[k]) / counts[k] - g) +
                                      sq(double(blue[k]) / counts[k] - b) <=
                                  1024) {
          used.insert(k);
          covered += counts[k];
        }
    }
    return total ? double(covered) / total : 0.;
  };
  auto matchesBoundary = [&](int y) {
    if (y < 0 || y >= h)
      return false;
    std::fill(boundaryEdges.begin(), boundaryEdges.end(), 0);
    std::fill(boundaryMatches.begin(), boundaryMatches.end(), 0);
    int matches = 0, edgeMatches = 0, lastR = 0, lastG = 0, lastB = 0;
    for (int x = 0; x < n; x++) {
      size_t p = (size_t(y) * w + x * step) * 4;
      int r = im.data[p], g = im.data[p + 1], b = im.data[p + 2], j = x * 3;
      if (x && (std::abs(r - lastR) > 23 || std::abs(g - lastG) > 23 || std::abs(b - lastB) > 23))
        boundaryEdges[x] = 1;
      if (x >= left && x <= right && std::abs(r - reference[j]) <= 20 &&
          std::abs(g - reference[j + 1]) <= 20 && std::abs(b - reference[j + 2]) <= 20) {
        matches++;
        boundaryMatches[x] = 1;
      }
      lastR = r;
      lastG = g;
      lastB = b;
    }
    for (int x = left; x <= right; x++)
      if (refEdge[x] && (boundaryEdges[x] || (x > 0 && boundaryEdges[x - 1]) ||
                         (x + 1 < n && boundaryEdges[x + 1])))
        edgeMatches++;
    return double(matches) / (right - left + 1) >= 2. / 3 && edgeMatches >= seedEdges * .5;
  };
  auto finish = [&]() {
    if (top < 0)
      return;
    if (bottom - top + 1 > std::max(32., w / 25.)) {
      top = -1;
      return;
    }
    int evidence = rows - 1;
    for (int x = left; x <= right; x++)
      good[x]--;
    auto add = [&]() {
      evidence++;
      for (int x = left; x <= right; x++)
        good[x] += boundaryMatches[x];
    };
    for (int i = 0; i < rowStep - 1 && matchesBoundary(top - 1); i++) {
      top--;
      add();
    }
    for (int i = 0; i < rowStep - 1 && matchesBoundary(bottom + 1); i++) {
      bottom++;
      add();
    }
    int height = bottom - top + 1, width = (right - left + 1) * step;
    double aspect = double(width) / height;
    if (height < 3 || height > std::max(32., w / 25.) || aspect < 20 || aspect > 80 ||
        evidence < 2) {
      top = -1;
      return;
    }
    int stable = 0;
    for (int x = left; x <= right; x++)
      if (good[x] >= evidence * .8)
        stable++;
    double ratio = double(stable) / (right - left + 1);
    if (ratio >= 2. / 3) {
      double upper =
          ratio * std::exp(-std::abs(std::log(aspect / 40))) * std::min(1., seedEdges / 24.);
      if (results.size() < 5 || upper > results.back().score) {
        double colors = colorCoverage(top, bottom, left, right);
        results.push_back({top, bottom, upper * (.2 + .8 * colors * colors)});
        std::stable_sort(results.begin(), results.end(),
                         [](auto a, auto b) { return a.score > b.score; });
        if (results.size() > 5)
          results.pop_back();
      }
    }
    top = -1;
  };
  for (int y = h - 1; y >= 0; y -= rowStep) {
    if (cancelled()) return {};
    std::fill(edge.begin(), edge.end(), 0);
    int first = n, last = -1, count = 0;
    for (int x = 0; x < n; x++) {
      size_t p = (size_t(y) * w + x * step) * 4;
      int j = x * 3, r = im.data[p], g = im.data[p + 1], b = im.data[p + 2];
      current[j] = r;
      current[j + 1] = g;
      current[j + 2] = b;
      if (x && (std::abs(r - current[j - 3]) > 23 || std::abs(g - current[j - 2]) > 23 ||
                std::abs(b - current[j - 1]) > 23)) {
        edge[x] = 1;
        first = std::min(first, x);
        last = x;
        count++;
      }
    }
    bool textured = count >= 6 && (last - first) * step >= std::min(120., w * .3);
    if (top >= 0) {
      int matches = 0, matchedEdges = 0;
      for (int x = left; x <= right; x++) {
        int j = x * 3;
        bool same = std::abs(int(current[j]) - reference[j]) <= 20 &&
                    std::abs(int(current[j + 1]) - reference[j + 1]) <= 20 &&
                    std::abs(int(current[j + 2]) - reference[j + 2]) <= 20;
        mask[x] = same;
        if (same)
          matches++;
        if (refEdge[x] && (edge[x] || (x > 0 && edge[x - 1]) || (x + 1 < n && edge[x + 1])))
          matchedEdges++;
      }
      if (!textured || double(matches) / (right - left + 1) < 2. / 3 ||
          matchedEdges < seedEdges * .5)
        finish();
    }
    if (top < 0 && textured) {
      top = bottom = y;
      left = first;
      right = last;
      rows = 0;
      seedEdges = count;
      reference = current;
      refEdge = edge;
      std::fill(good.begin(), good.end(), 0);
      std::fill(mask.begin(), mask.end(), 1);
    }
    if (top >= 0) {
      top = y;
      rows++;
      for (int x = left; x <= right; x++)
        good[x] += mask[x];
    }
  }
  finish();
  return results;
}
} // namespace pidan
