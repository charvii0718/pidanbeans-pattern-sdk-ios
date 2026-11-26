// Ported from packages/watermark-recognizer/src/watermarkSampling.ts; TS remains the regression
// oracle.
#include "internal.hpp"
namespace pidan {
using Colors = std::array<RGB, COLS>;
struct Reference {
  std::array<RGB, 2> colors;
  double separation, quality;
  int offset;
};
static double dist(RGB a, RGB b) {
  return sq(a[0] - b[0]) + sq(a[1] - b[1]) + sq(a[2] - b[2]);
}
static double lum(RGB a) {
  return (77 * a[0] + 150 * a[1] + 29 * a[2]) / 256;
}
static double radius(const Reference &r) {
  return std::max(14., r.separation * .28);
}
static RGB median(const std::vector<RGB> &colors) {
  RGB result{};
  for (int c = 0; c < 3; c++) {
    Line values;
    for (auto rgb : colors)
      values.push_back(rgb[c]);
    std::sort(values.begin(), values.end());
    int m = int(values.size() / 2);
    result[c] = values.size() % 2 ? values[m] : (values[m - 1] + values[m]) / 2;
  }
  return result;
}
static Colors rowColors(const Image &im, double x, int y, double cell) {
  Colors values{};
  int width = std::max(1, rnd(cell * .5));
  for (int i = 0; i < COLS; i++) {
    int start = rnd(x + i * cell + rnd(cell * .25)), n = 0;
    RGB rgb{};
    for (int j = 0; j < width; j++) {
      int gx = start + j;
      if (gx < 0 || gx >= im.w || y < 0 || y >= im.h)
        continue;
      size_t p = (size_t(y) * im.w + gx) * 4;
      for (int c = 0; c < 3; c++)
        rgb[c] += im.data[p + c];
      n++;
    }
    for (int c = 0; c < 3; c++)
      values[i][c] = n ? rgb[c] / n : 255;
  }
  return values;
}
static std::optional<Reference> reference(const Colors &values, int offset) {
  const auto &mark = offset ? END : HEAD;
  std::array<std::vector<RGB>, 2> groups;
  for (int i = 0; i < 8; i++)
    groups[mark[i] - 1].push_back(values[offset + i]);
  std::array<RGB, 2> colors = {median(groups[0]), median(groups[1])};
  double separation = std::sqrt(dist(colors[0], colors[1]));
  if (separation < 35 || lum(colors[0]) - lum(colors[1]) < 25)
    return {};
  double tolerance = sq(std::max(14., separation * .28)), spread = 0;
  int good = 0, errors = 0;
  for (int i = 0; i < 8; i++) {
    int expected = mark[i] - 1;
    auto rgb = values[offset + i];
    double own = dist(rgb, colors[expected]), other = dist(rgb, colors[1 - expected]);
    if (other < own)
      errors++;
    if (own <= tolerance) {
      good++;
      spread += own / sq(separation);
    }
  }
  if (good < 6 || errors > 1)
    return {};
  return Reference{colors, separation, good - errors * .5 - spread, offset};
}
static bool compatible(const Reference &a, const Reference &b) {
  double tolerance = sq(std::max(14., std::min(a.separation, b.separation) * .2));
  return dist(a.colors[0], b.colors[0]) <= tolerance && dist(a.colors[1], b.colors[1]) <= tolerance;
}
static std::optional<Reference> selectColors(const std::vector<Colors> &rows, int preferred) {
  std::vector<Reference> candidates;
  for (const auto &row : rows)
    for (int offset : {0, TAIL}) {
      auto ref = reference(row, offset);
      if (!ref)
        continue;
      auto dup = std::find_if(candidates.begin(), candidates.end(),
                              [&](const auto &c) { return compatible(c, *ref); });
      if (dup == candidates.end())
        candidates.push_back(*ref);
      else if (ref->quality > dup->quality)
        *dup = *ref;
    }
  std::stable_sort(candidates.begin(), candidates.end(),
                   [](auto a, auto b) { return a.quality > b.quality; });
  std::optional<Reference> best;
  double bestScore = -1e100;
  for (size_t i = 0; i < std::min(size_t(12), candidates.size()); i++) {
    auto ref = candidates[i];
    double tolerance = sq(radius(ref));
    std::array<double, 2> support{};
    for (const auto &row : rows)
      for (auto rgb : row) {
        double d0 = dist(rgb, ref.colors[0]), d1 = dist(rgb, ref.colors[1]);
        support[d0 <= d1 ? 0 : 1] += std::max(0., 1 - std::min(d0, d1) / tolerance);
      }
    double score = support[0] + support[1] + std::min(support[0], support[1]) * .5 +
                   ref.quality * .01 + (ref.offset == preferred ? .001 : 0);
    if (score > bestScore) {
      best = ref;
      bestScore = score;
    }
  }
  if (!best)
    return {};
  std::array<std::vector<RGB>, 2> groups;
  double tolerance = sq(radius(*best));
  for (const auto &row : rows)
    for (int offset : {0, TAIL}) {
      auto ref = reference(row, offset);
      if (!ref || !compatible(*best, *ref))
        continue;
      const auto &mark = offset ? END : HEAD;
      for (int i = 0; i < 8; i++) {
        int expected = mark[i] - 1;
        auto rgb = row[offset + i];
        if (dist(rgb, best->colors[expected]) <= tolerance)
          groups[expected].push_back(rgb);
      }
    }
  if (groups[0].size() >= 3 && groups[1].size() >= 3) {
    best->colors = {median(groups[0]), median(groups[1])};
    best->separation = std::sqrt(dist(best->colors[0], best->colors[1]));
  }
  return best;
}
std::pair<int, int> bounds(const Image &im, const Geometry &g, int seed) {
  auto fallback =
      std::make_pair(std::max(0, seed - 1), std::min(im.h, seed + 2) - std::max(0, seed - 1));
  std::vector<Colors> rows;
  for (int y = std::max(0, seed - 1); y <= std::min(im.h - 1, seed + 1); y++)
    rows.push_back(rowColors(im, g.x, y, g.cell));
  auto ref = selectColors(rows, g.offset);
  if (!ref)
    return fallback;
  std::vector<int> indices;
  for (int i = 0; i < 8; i++)
    indices.push_back(i);
  for (int i = TAIL; i < COLS; i++)
    indices.push_back(i);
  for (int i = 8; i < TAIL; i += 4)
    indices.push_back(i);
  int start = std::max(0, int(std::floor(seed - g.cell * 3))),
      end = std::min(im.h, int(std::ceil(seed + g.cell * 3 + 1)));
  Bytes valid(end - start);
  double tolerance = sq(radius(*ref));
  for (int y = start; y < end; y++) {
    if (cancelled()) return {};
    auto values = rowColors(im, g.x, y, g.cell);
    auto match = [&](int offset) {
      const auto &mark = offset ? END : HEAD;
      int good = 0, errors = 0;
      for (int i = 0; i < 8; i++) {
        int expected = mark[i] - 1;
        double own = dist(values[offset + i], ref->colors[expected]),
               other = dist(values[offset + i], ref->colors[1 - expected]);
        if (other < own)
          errors++;
        if (own <= tolerance)
          good++;
      }
      return good >= 6 && errors <= 1;
    };
    if (!match(0) && !match(TAIL))
      continue;
    int support = 0;
    for (int i : indices)
      if (std::min(dist(values[i], ref->colors[0]), dist(values[i], ref->colors[1])) <= tolerance)
        support++;
    if (support >= indices.size() * .45)
      valid[y - start] = 1;
  }
  for (int i = 1; i < int(valid.size()) - 1; i++)
    if (!valid[i] && valid[i - 1] && valid[i + 1])
      valid[i] = 1;
  int bestStart = -1, bestEnd = -1;
  double bestDistance = 1e100;
  for (int i = 0; i < int(valid.size());) {
    if (!valid[i]) {
      i++;
      continue;
    }
    int a = i;
    while (i < int(valid.size()) && valid[i])
      i++;
    int b = i;
    double distance = std::max({double(start + a - seed), double(seed - (start + b - 1)), 0.});
    if (b - a >= 2 && distance <= std::max(2., g.cell * .5) &&
        (distance < bestDistance || (distance == bestDistance && b - a > bestEnd - bestStart))) {
      bestStart = a;
      bestEnd = b;
      bestDistance = distance;
    }
  }
  if (bestStart < 0)
    return fallback;
  int top = start + bestStart, bottom = start + bestEnd, maxHeight = int(std::ceil(g.cell * 3 + 2));
  if (bottom - top > maxHeight) {
    top = std::max(top, std::min(bottom - maxHeight, seed - maxHeight / 2));
    bottom = top + maxHeight;
  }
  return {top, bottom - top};
}
std::optional<std::array<int, COLS>> sample(const Image &im, const Geometry &g) {
  if (!std::isfinite(g.cell) || g.cell < 2)
    return {};
  int start = std::max(0, rnd(g.y)),
      end = std::min(im.h, rnd(g.y + (g.height ? g.height : g.cell * 3)));
  if (end <= start)
    return {};
  std::vector<Colors> rows;
  for (int y = start; y < end; y++)
    rows.push_back(rowColors(im, g.x, y, g.cell));
  auto ref = selectColors(rows, g.offset);
  if (!ref)
    return {};
  double tolerance = sq(radius(*ref));
  std::array<int, COLS> cells{};
  for (int i = 0; i < COLS; i++) {
    std::vector<int> bits;
    Line weights;
    for (const auto &row : rows) {
      double d0 = dist(row[i], ref->colors[0]), d1 = dist(row[i], ref->colors[1]);
      bits.push_back(d0 <= d1 ? 0 : 1);
      weights.push_back(sq(std::max(0., 1 - std::min(d0, d1) / tolerance)));
    }
    std::array<double, 2> votes{};
    for (size_t j = 0; j < rows.size(); j++) {
      double neighbor =
          std::max(j && bits[j - 1] == bits[j] ? weights[j - 1] : 0.,
                   j + 1 < rows.size() && bits[j + 1] == bits[j] ? weights[j + 1] : 0.);
      votes[bits[j]] += weights[j] * (.2 + .8 * neighbor);
    }
    if (votes[0] + votes[1] > 0)
      cells[i] = votes[0] >= votes[1] ? 1 : 2;
    else {
      std::vector<RGB> values;
      for (const auto &row : rows)
        values.push_back(row[i]);
      auto rgb = median(values);
      cells[i] = dist(rgb, ref->colors[0]) <= dist(rgb, ref->colors[1]) ? 1 : 2;
    }
  }
  return cells;
}
} // namespace pidan
