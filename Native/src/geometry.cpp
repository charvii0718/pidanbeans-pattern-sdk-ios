// Ported from packages/watermark-recognizer/src/watermarkRecognition.ts / watermarkYRecognition.ts;
// TS remains the regression oracle.
#include "internal.hpp"
namespace pidan {
std::optional<Fit> fitLine(const std::vector<Point> &points, double minCell) {
  if (points.size() < 2)
    return {};
  double k = 0, x = 0, num = 0, den = 0;
  for (auto p : points) {
    k += p.k;
    x += p.x;
  }
  k /= points.size();
  x /= points.size();
  for (auto p : points) {
    num += (p.k - k) * (p.x - x);
    den += sq(p.k - k);
  }
  double cell = num / den;
  if (!std::isfinite(cell) || cell < minCell)
    return {};
  return Fit{x - k * cell, cell, 0};
}
std::vector<Edge> edges(const Line &line, double mergeCell) {
  Line gradient(line.size());
  for (int x = 1; x < int(line.size()) - 1; x++)
    gradient[x] = (line[x + 1] - line[x - 1]) / 2;
  std::vector<Edge> result;
  for (int x = 2; x < int(line.size()) - 2; x++) {
    double value = std::abs(gradient[x]);
    if (value < 12 || value < std::abs(gradient[x - 1]) || value <= std::abs(gradient[x + 1]))
      continue;
    double left = std::abs(gradient[x - 1]), right = std::abs(gradient[x + 1]),
           den = left - 2 * value + right,
           delta = den ? std::max(-.5, std::min(.5, (left - right) / (2 * den))) : 0;
    Edge e{x + delta + .5, gradient[x]};
    if (!result.empty() && e.x - result.back().x < mergeCell * .45) {
      if (value > std::abs(result.back().strength))
        result.back() = e;
    } else
      result.push_back(e);
  }
  return result;
}
std::vector<Anchor> exactAnchors(const Line &line, const std::vector<Edge> &e, int offset,
                                 double coarse, bool yMode, int row) {
  std::vector<Anchor> result;
  const auto &mark = offset ? END : HEAD;
  double minCell = yMode ? std::max(2., coarse * .65) : coarse * .85,
         maxCell = yMode ? std::min(line.size() / 120. * 1.02, coarse * 1.5) : coarse * 1.15;
  for (int variant = 0; variant < 9; variant++) {
    if (cancelled()) return {};
    auto cells = mark;
    if (variant)
      cells[variant - 1] = 3 - cells[variant - 1];
    std::vector<int> keys;
    for (int i = 1; i < 8; i++)
      if (cells[i] != cells[i - 1])
        keys.push_back(i);
    if (keys.size() < 2 || (!yMode && keys.back() - keys.front() < 3))
      continue;
    int first = keys.front(), last = keys.back();
    for (int outer = offset && yMode ? int(e.size()) - 1 : 0; outer >= 0 && outer < int(e.size());
         outer += offset && yMode ? -1 : 1) {
      bool tail = offset && yMode;
      if ((e[outer].strength < 0) != (cells[tail ? last : first] == 2))
        continue;
      for (int inner = outer + (tail ? -1 : 1); inner >= 0 && inner < int(e.size());
           inner += tail ? -1 : 1) {
        int a = tail ? inner : outer, b = tail ? outer : inner;
        double cell = (e[b].x - e[a].x) / (last - first);
        if (cell < minCell)
          continue;
        if (cell > maxCell)
          break;
        if ((e[inner].strength < 0) != (cells[tail ? first : last] == 2))
          continue;
        double origin = e[a].x - first * cell;
        std::vector<Point> points;
        for (int key : keys) {
          double target = origin + key * cell;
          const Edge *best = nullptr;
          auto it = std::lower_bound(e.begin() + a, e.begin() + b + 1, target - cell * .28,
                                     [](auto edge, double x) { return edge.x < x; });
          for (; it != e.begin() + b + 1 && it->x <= target + cell * .28; ++it)
            if ((it->strength < 0) == (cells[key] == 2)) {
              if (!best || std::abs(it->x - target) < std::abs(best->x - target))
                best = &*it;
              if (!yMode)
                break;
            }
          if (!best)
            break;
          points.push_back({double(key), best->x});
        }
        if (points.size() != keys.size())
          continue;
        auto fit = fitLine(points, yMode ? 2 : 0);
        if (!fit || fit->x < -.5 || fit->x + 8 * fit->cell > line.size() + .5 ||
            (yMode && (fit->cell < minCell || fit->cell > maxCell)))
          continue;
        double error = 0, maxError = 0, bright = 255, dark = 0;
        for (auto p : points) {
          double v = std::abs(p.x - fit->x - p.k * fit->cell) / fit->cell;
          error += v * v;
          maxError = std::max(maxError, v);
        }
        error = yMode ? maxError : std::sqrt(error / points.size());
        if (error > (yMode ? .25 : .15))
          continue;
        for (int k = 0; k < 8; k++) {
          double value =
              line[clamp(rnd(fit->x + (k + .5) * fit->cell - .5), 0, int(line.size()) - 1)];
          if (cells[k] == 1)
            bright = std::min(bright, value);
          else
            dark = std::max(dark, value);
        }
        if (bright - dark < 35)
          continue;
        Anchor anchor;
        anchor.x = fit->x;
        anchor.cell = fit->cell;
        anchor.error = error;
        anchor.bad = variant ? 1 : 0;
        anchor.offset = offset;
        anchor.row = row;
        for (auto p : points)
          anchor.points.push_back({p.k + offset, p.x});
        result.push_back(anchor);
      }
    }
  }
  std::stable_sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
    return a.bad != b.bad ? a.bad < b.bad : a.error < b.error;
  });
  std::vector<Anchor> unique;
  for (const auto &a : result) {
    bool dup = false;
    for (auto &b : unique)
      if (std::abs(a.x - b.x) < (yMode ? a.cell * .3 : coarse * .2) &&
          std::abs(a.cell - b.cell) * 8 < (yMode ? a.cell * .3 : coarse * .2)) {
        dup = true;
        break;
      }
    if (!dup)
      unique.push_back(a);
  }
  if (yMode) {
    std::vector<Anchor> full, damaged;
    for (auto a : unique)
      (a.bad ? damaged : full).push_back(a);
    std::stable_sort(damaged.begin(), damaged.end(),
                     [&](auto a, auto b) { return offset ? a.x > b.x : a.x < b.x; });
    if (damaged.size() > 3)
      damaged.resize(3);
    full.insert(full.end(), damaged.begin(), damaged.end());
    return full;
  }
  return unique;
}
} // namespace pidan
