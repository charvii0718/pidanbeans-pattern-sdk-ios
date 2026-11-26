// Ported from packages/watermark-recognizer/src/watermarkYRecognition.ts; TS remains the regression
// oracle.
#include "internal.hpp"
namespace pidan {
struct Row {
  int y;
  Line line;
  std::vector<Edge> e;
};
static bool inImage(double x, double cell, int w) {
  return x >= -.5 && cell >= 2 && x + COLS * cell <= w + .5;
}
static Row readRow(const Image &im, int y) {
  Row r;
  r.y = y;
  r.line.resize(im.w);
  for (int x = 0; x < im.w; x++)
    for (int dy = -1; dy <= 1; dy++) {
      size_t p = (size_t(clamp(y + dy, 0, im.h - 1)) * im.w + x) * 4;
      r.line[x] += (im.data[p] * 77 + im.data[p + 1] * 150 + im.data[p + 2] * 29) / 768.;
    }
  r.e = edges(r.line);
  return r;
}
static double colorPenalty(const Geometry &fit, const Row &row) {
  const auto &mark = fit.offset ? END : HEAD;
  Line bright, dark;
  auto at = [&](int k) {
    return row.line[clamp(rnd(fit.x + (k + .5) * fit.cell - .5), 0, int(row.line.size()) - 1)];
  };
  for (int i = 0; i < 8; i++)
    (mark[i] == 1 ? bright : dark).push_back(at(fit.offset + i));
  std::sort(bright.begin(), bright.end());
  std::sort(dark.begin(), dark.end());
  double b = (bright[1] + bright[2]) / 2, d = (dark[1] + dark[2]) / 2,
         tolerance = std::max(14., std::abs(b - d) * .28);
  int supported = 0;
  for (int k = 0; k < COLS; k++)
    if (std::min(std::abs(at(k) - b), std::abs(at(k) - d)) <= tolerance)
      supported++;
  return (1 - supported / 120.) * 8;
}
static std::optional<Fit> calibrate(const std::vector<Point> &points, Fit current, const Row &row) {
  double error = 0;
  for (int pass = 0; pass < 2; pass++) {
    std::vector<Point> aligned;
    int near = 0;
    for (auto e : row.e)
      if (e.x >= current.x && e.x <= current.x + COLS * current.cell) {
        near++;
        double k = rnd((e.x - current.x) / current.cell);
        if (std::abs(e.x - current.x - k * current.cell) < current.cell * .25)
          aligned.push_back({k, e.x});
      }
    if (aligned.size() < 8 || aligned.size() / double(std::max(1, near)) < .55 ||
        aligned.back().x - aligned.front().x < COLS * current.cell * .7)
      return {};
    auto all = aligned;
    all.insert(all.end(), points.begin(), points.end());
    all.insert(all.end(), points.begin(), points.end());
    auto fit = fitLine(all);
    if (!fit)
      return {};
    for (auto p : points)
      if (std::abs(p.x - fit->x - p.k * fit->cell) > fit->cell * .35)
        return {};
    current = *fit;
    error = 0;
    for (auto p : aligned)
      error += sq((p.x - fit->x - p.k * fit->cell) / fit->cell);
    error /= aligned.size();
  }
  current.error = error;
  return inImage(current.x, current.cell, int(row.line.size())) ? std::optional<Fit>(current)
                                                                : std::nullopt;
}
static std::optional<Geometry> singleGeometry(const Anchor &a, const Row &row) {
  struct Measure : Fit {
    double ratio = 0, span = 0, score = 0;
    int count = 0;
  };
  auto measure = [&](double x, double cell) -> std::optional<Measure> {
    if (!inImage(x, cell, int(row.line.size())))
      return {};
    for (auto p : a.points)
      if (std::abs(p.x - x - p.k * cell) > cell * .35)
        return {};
    int total = 0, count = 0;
    double first = 0, last = 0, error = 0;
    for (auto e : row.e) {
      if (e.x < x || e.x > x + COLS * cell)
        continue;
      total++;
      double delta = (e.x - x) / cell - rnd((e.x - x) / cell);
      if (std::abs(delta) >= .25)
        continue;
      if (!count)
        first = e.x;
      last = e.x;
      count++;
      error += delta * delta;
    }
    Measure m;
    m.x = x;
    m.cell = cell;
    m.count = count;
    m.ratio = double(count) / std::max(1, total);
    m.span = (last - first) / (COLS * cell);
    m.error = error / std::max(1, count);
    m.score = m.ratio - m.error;
    return m;
  };
  std::optional<Measure> best;
  for (int i = 0; i < 33; i++) {
    double cell = a.cell * (1 + (i - 16) * .0025), x = 0;
    for (auto p : a.points)
      x += p.x - p.k * cell;
    x /= a.points.size();
    auto fit = measure(x, cell);
    if (!fit || fit->count < 16 || fit->span < .7)
      continue;
    if (!best || fit->score > best->score)
      best = fit;
  }
  if (!best)
    return {};
  std::vector<Point> points;
  for (auto e : row.e)
    if (e.x >= best->x && e.x <= best->x + COLS * best->cell) {
      double k = rnd((e.x - best->x) / best->cell);
      if (std::abs(e.x - best->x - k * best->cell) < best->cell * .25)
        points.push_back({k, e.x});
    }
  points.insert(points.end(), a.points.begin(), a.points.end());
  points.insert(points.end(), a.points.begin(), a.points.end());
  auto refined = fitLine(points);
  if (refined && std::abs(refined->cell / a.cell - 1) <= .04) {
    auto fit = measure(refined->x, refined->cell);
    if (fit && fit->count >= 16 && fit->span >= .7 && fit->score > best->score)
      best = fit;
  }
  if (best->ratio < .65)
    return {};
  Geometry g;
  g.x = best->x;
  g.cell = best->cell;
  g.row = a.row;
  g.offset = a.offset;
  g.score = a.bad * 4 + a.error + best->error;
  return g;
}
std::optional<pidan_watermark> scanY(const Image &small, const Image &large, double inv, int yOff) {
  auto bands = locateY(small);
  for (size_t index = 0; index < std::min(size_t(3), bands.size()); index++) {
    if (cancelled()) return {};
    auto band = bands[index];
    double bandTop = (band.top + yOff) * inv, bandBottom = (band.bottom + 1 + yOff) * inv - 1;
    int margin = std::max(3, int(std::ceil((bandBottom - bandTop + 1) / 2))),
        top = std::max(0, int(std::floor(bandTop - margin))),
        height = std::min(large.h - top, int(std::ceil(bandBottom + margin - top + 1)));
    if (height < 3)
      continue;
    Image roi = resize(large, 0, top, large.w, height, large.w, height);
    double localTop = bandTop - top, localBottom = bandBottom - top,
           bh = localBottom - localTop + 1, coarse = bh / 3;
    std::vector<Row> rows;
    std::set<int> visited;
    for (double fraction : {.5, .25, .75}) {
      int y = clamp(rnd(localTop + (bh - 1) * fraction), 1, height - 2);
      if (visited.insert(y).second)
        rows.push_back(readRow(roi, y));
    }
    std::vector<Anchor> heads, tails;
    for (int r = 0; r < int(rows.size()); r++) {
    if (cancelled()) return {};
      auto h = exactAnchors(rows[r].line, rows[r].e, 0, coarse, true, r),
           t = exactAnchors(rows[r].line, rows[r].e, TAIL, coarse, true, r);
      heads.insert(heads.end(), h.begin(), h.end());
      tails.insert(tails.end(), t.begin(), t.end());
    }
    auto retain = [](std::vector<Anchor> all, bool tail) {
      std::stable_sort(all.begin(), all.end(), [](const auto &a, const auto &b) {
        return a.bad != b.bad ? a.bad < b.bad : a.error < b.error;
      });
      std::vector<Anchor> unique, damaged;
      for (auto a : all) {
        bool dup = false;
        for (auto b : unique)
          if (std::abs(a.x - b.x) < a.cell * .3 && std::abs(a.cell - b.cell) * 8 < a.cell * .3)
            dup = true;
        for (auto b : damaged)
          if (std::abs(a.x - b.x) < a.cell * .3 && std::abs(a.cell - b.cell) * 8 < a.cell * .3)
            dup = true;
        if (!dup)
          (a.bad ? damaged : unique).push_back(a);
      }
      std::stable_sort(damaged.begin(), damaged.end(),
                       [&](auto a, auto b) { return tail ? a.x > b.x : a.x < b.x; });
      if (damaged.size() > 3)
        damaged.resize(3);
      unique.insert(unique.end(), damaged.begin(), damaged.end());
      return unique;
    };
    heads = retain(heads, false);
    tails = retain(tails, true);
    if (heads.empty() && tails.empty())
      continue;
    auto geometries = [&](bool single) {
      std::vector<Geometry> fits;
      auto add = [&](Geometry g) {
        g.score += colorPenalty(g, rows[g.row]);
        auto old = std::find_if(fits.begin(), fits.end(), [&](auto f) {
          return std::abs(f.x - g.x) < g.cell * .3 &&
                 std::abs(f.cell - g.cell) * TAIL < g.cell * .3;
        });
        if (old == fits.end())
          fits.push_back(g);
        else if (g.score < old->score)
          *old = g;
      };
      if (!single)
        for (auto h : heads)
          for (auto t : tails) {
            double cell = (t.x - h.x) / TAIL;
            if (!inImage(h.x, cell, roi.w) || std::abs(h.cell - cell) > cell * .12 ||
                std::abs(t.cell - cell) > cell * .12)
              continue;
            auto points = h.points;
            points.insert(points.end(), t.points.begin(), t.points.end());
            auto joint = fitLine(points);
            if (!joint)
              continue;
            auto preferred = h.bad <= t.bad ? h : t;
            auto fit = calibrate(points, *joint, rows[preferred.row]);
            if (fit) {
              Geometry g;
              g.x = fit->x;
              g.cell = fit->cell;
              g.row = preferred.row;
              g.offset = preferred.offset;
              g.score = (h.bad + t.bad) * 4 + h.error + t.error + fit->error;
              add(g);
            }
          }
      if (single) {
        auto all = heads;
        all.insert(all.end(), tails.begin(), tails.end());
        for (auto a : all) {
          auto fit = singleGeometry(a, rows[a.row]);
          if (fit)
            add(*fit);
        }
      }
      std::stable_sort(fits.begin(), fits.end(), [](auto a, auto b) { return a.score < b.score; });
      return fits;
    };
    int attempts = 0;
    std::set<std::string> decoded;
    auto tryFits = [&](const std::vector<Geometry> &fits, int begin,
                       int limit) -> std::optional<pidan_watermark> {
      for (int i = begin; i < int(fits.size()) && i < begin + limit; i++) {
    if (cancelled()) return {};
        attempts++;
        auto g = fits[i];
        int seed = rows[g.row].y;
        auto vertical = bounds(roi, g, seed);
        g.y = vertical.first;
        g.height = vertical.second;
        auto cells = sample(roi, g);
        if (!cells)
          continue;
        std::string key;
        for (int v : *cells)
          key += char('0' + v);
        if (!decoded.insert(key).second)
          continue;
        auto result = decode(*cells);
        if (result) {
          result->method = 3;
          return result;
        }
      }
      return {};
    };
    auto doubles = geometries(false);
    auto result = tryFits(doubles, 0, 2);
    if (result)
      return result;
    auto singles = geometries(true);
    result = tryFits(singles, 0, 3 - attempts);
    if (result)
      return result;
    if (attempts < 3) {
      result = tryFits(doubles, 2, 3 - attempts);
      if (result)
        return result;
    }
  }
  return {};
}
} // namespace pidan
