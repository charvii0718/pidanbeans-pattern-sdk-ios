// Ported from packages/watermark-recognizer/src/watermarkBandLocator.ts / watermarkRecognition.ts;
// TS remains the regression oracle.
#include "internal.hpp"
namespace pidan {
static std::optional<Fit> centered(const std::vector<Point> &points, int width) {
  double num = 0, den = 0;
  for (auto p : points) {
    double k = p.k - COLS / 2.;
    num += k * (p.x - width / 2.);
    den += k * k;
  }
  if (!den)
    return {};
  double cell = num / den;
  if (!std::isfinite(cell) || cell < 2)
    return {};
  Fit fit{(width - COLS * cell) / 2, cell, 0};
  for (auto p : points) {
    double error = std::abs(p.x - fit.x - p.k * cell) / cell;
    if (error > .3)
      return {};
    fit.error += error * error;
  }
  fit.error = std::sqrt(fit.error / points.size());
  return fit;
}
struct Support {
  std::vector<Point> points;
  double error;
};
static std::optional<Support> support(const Fit &fit, const std::vector<Edge> &e, bool single) {
  Support s;
  int near = 0;
  s.error = 0;
  for (auto edge : e)
    if (edge.x >= fit.x && edge.x <= fit.x + COLS * fit.cell) {
      near++;
      double k = rnd((edge.x - fit.x) / fit.cell);
      if (std::abs(edge.x - fit.x - k * fit.cell) < fit.cell * .25)
        s.points.push_back({k, edge.x});
    }
  if (s.points.size() < 16 || s.points.size() / double(std::max(1, near)) < (single ? .65 : .55) ||
      s.points.back().x - s.points.front().x < COLS * fit.cell * (single ? .75 : .65))
    return {};
  for (auto p : s.points)
    s.error += sq((p.x - fit.x - p.k * fit.cell) / fit.cell);
  s.error = std::sqrt(s.error / s.points.size());
  return s;
}
static std::optional<Fit> validate(const std::vector<Anchor> &anchors, const std::vector<Edge> &e,
                                   int width, double coarse) {
  std::vector<Point> points;
  for (auto a : anchors)
    points.insert(points.end(), a.points.begin(), a.points.end());
  auto fit = centered(points, width);
  if (!fit || fit->x < -.5 || fit->cell < coarse * .85 || fit->cell > coarse * 1.15)
    return {};
  auto sup = support(*fit, e, anchors.size() == 1);
  if (!sup)
    return {};
  if (anchors.size() == 1)
    for (int pass = 0; pass < 2; pass++) {
      auto all = points;
      all.insert(all.end(), points.begin(), points.end());
      all.insert(all.end(), sup->points.begin(), sup->points.end());
      fit = centered(all, width);
      if (!fit)
        return {};
      for (auto p : points)
        if (std::abs(p.x - fit->x - p.k * fit->cell) > fit->cell * .3)
          return {};
      sup = support(*fit, e, true);
      if (!sup)
        return {};
    }
  fit->error += sup->error;
  return fit;
}
static double contrast(const Line &line, const Fit &fit, int offset) {
  const auto &mark = offset ? END : HEAD;
  Line bright, dark;
  for (int i = 0; i < 8; i++) {
    double v = line[clamp(rnd(fit.x + (offset + i + .5) * fit.cell - .5), 0, int(line.size()) - 1)];
    (mark[i] == 1 ? bright : dark).push_back(v);
  }
  std::sort(bright.begin(), bright.end());
  std::sort(dark.begin(), dark.end());
  return (bright[1] + bright[2] - dark[1] - dark[2]) / 2;
}
static std::vector<Anchor> centeredAnchors(const Line &line, const std::vector<Edge> &e, int offset,
                                           double minCell) {
  std::vector<Anchor> result;
  const auto &mark = offset ? END : HEAD;
  int width = int(line.size());
  for (int variant = 0; variant < 9; variant++) {
    if (cancelled()) return {};
    auto cells = mark;
    if (variant)
      cells[variant - 1] = 3 - cells[variant - 1];
    std::vector<int> keys;
    for (int i = 1; i < 8; i++)
      if (cells[i] != cells[i - 1])
        keys.push_back(i);
    if (keys.empty())
      continue;
    int first = keys[0];
    for (auto edge : e) {
      if ((edge.strength < 0) != (cells[first] == 2))
        continue;
      double cell = (edge.x - width / 2.) / (offset + first - COLS / 2.);
      if (cell < minCell * .95 || cell > width / 120. * 1.02)
        continue;
      double origin = (width - COLS * cell) / 2, low = 255, high = 0;
      Line rough(8);
      for (int i = 0; i < 8; i++) {
        rough[i] = line[clamp(rnd(origin + (offset + i + .5) * cell - .5), 0, width - 1)];
        low = std::min(low, rough[i]);
        high = std::max(high, rough[i]);
      }
      if (high - low < 35)
        continue;
      double middle = (low + high) / 2, bright = 255, dark = 0;
      bool match = true;
      for (int i = 0; i < 8; i++) {
        if ((rough[i] < middle ? 2 : 1) != cells[i])
          match = false;
        if (cells[i] == 1)
          bright = std::min(bright, rough[i]);
        else
          dark = std::max(dark, rough[i]);
      }
      if (!match || bright - dark < 25)
        continue;
      std::vector<Point> points;
      for (int key : keys) {
        double k = offset + key, target = origin + k * cell;
        auto it = std::lower_bound(e.begin(), e.end(), target - cell * .28,
                                   [](auto edge, double x) { return edge.x < x; });
        for (; it != e.end() && it->x < target + cell * .28; it++)
          if ((it->strength < 0) == (cells[key] == 2))
            break;
        if (it == e.end() || it->x >= target + cell * .28)
          break;
        points.push_back({k, it->x});
      }
      if (points.size() != keys.size())
        continue;
      auto fit = centered(points, width);
      if (!fit || fit->x < -.5 || fit->error > .15)
        continue;
      bright = 255;
      dark = 0;
      for (int i = 0; i < 8; i++) {
        double v = line[clamp(rnd(fit->x + (offset + i + .5) * fit->cell - .5), 0, width - 1)];
        if (cells[i] == 1)
          bright = std::min(bright, v);
        else
          dark = std::max(dark, v);
      }
      if (bright - dark < 35)
        continue;
      Anchor a;
      a.x = fit->x + offset * fit->cell;
      a.cell = fit->cell;
      a.error = fit->error;
      a.offset = offset;
      a.bad = variant ? 1 : 0;
      a.points = points;
      auto dup = std::find_if(result.begin(), result.end(), [&](auto b) {
        return std::abs(a.x - b.x) < cell * .2 && std::abs(a.cell - b.cell) * 8 < cell * .2;
      });
      if (dup == result.end())
        result.push_back(a);
      else if (a.bad < dup->bad || (a.bad == dup->bad && a.error < dup->error))
        *dup = a;
    }
  }
  std::stable_sort(result.begin(), result.end(), [](auto a, auto b) {
    return a.bad != b.bad ? a.bad < b.bad : a.error < b.error;
  });
  return result;
}
static std::optional<Geometry> locateRow(const Line &line) {
  auto e = edges(line, 3);
  auto heads = centeredAnchors(line, e, 0, 3), tails = centeredAnchors(line, e, TAIL, 3);
  std::vector<Geometry> fits;
  auto add = [&](std::vector<Anchor> anchors) {
    double coarse = 0;
    int bad = anchors.size() == 1 ? 2 : 0;
    for (auto a : anchors) {
      coarse += a.cell;
      bad += a.bad;
    }
    coarse /= anchors.size();
    auto fit = validate(anchors, e, int(line.size()), coarse);
    if (!fit || fit->cell < 2.85)
      return;
    auto best = anchors[0];
    for (auto a : anchors)
      if (a.bad < best.bad)
        best = a;
    Geometry g;
    g.x = fit->x;
    g.cell = std::max(3., fit->cell);
    g.offset = best.offset;
    g.score = bad * 10 + fit->error - contrast(line, *fit, best.offset) / 255;
    auto dup = std::find_if(fits.begin(), fits.end(), [&](auto f) {
      return std::abs(f.x - g.x) < fit->cell * .25 &&
             std::abs(f.cell - g.cell) * TAIL < fit->cell * .25;
    });
    if (dup == fits.end())
      fits.push_back(g);
    else if (g.score < dup->score)
      *dup = g;
  };
  for (auto h : heads)
    for (auto t : tails)
      if (t.x > h.x)
        add({h, t});
  if (fits.empty()) {
    for (auto h : heads)
      add({h});
    for (auto t : tails)
      add({t});
  }
  std::stable_sort(fits.begin(), fits.end(), [](auto a, auto b) { return a.score < b.score; });
  if (fits.empty() || (fits.size() > 1 && fits[1].score - fits[0].score < .05))
    return {};
  return fits[0];
}
static std::optional<Geometry> locateRegion(const Image &im, const Geometry &coarse) {
  std::vector<Geometry> geometries;
  std::set<int> visited;
  int lineIndex = 0;
  for (double offset : {0., -1., 1., -.5, .5, -1.5, 1.5}) {
    if (cancelled()) return {};
    if (lineIndex++ == 3 && !geometries.empty())
      break;
    double sampleY = coarse.y + offset * coarse.cell;
    int centerY = rnd(sampleY + coarse.cell);
    if (centerY < 1 || centerY >= im.h - 1 || !visited.insert(centerY).second)
      continue;
    Line line(im.w);
    for (int x = 0; x < im.w; x++)
      for (int dy = -1; dy <= 1; dy++) {
        size_t p = (size_t(centerY + dy) * im.w + x) * 4;
        line[x] += (im.data[p] * 77 + im.data[p + 1] * 150 + im.data[p + 2] * 29) / 768.;
      }
    auto e = edges(line, coarse.cell);
    auto heads = exactAnchors(line, e, 0, coarse.cell, false),
         tails = exactAnchors(line, e, TAIL, coarse.cell, false);
    std::vector<Geometry> fits;
    auto add = [&](const std::vector<Anchor> &anchors) {
      auto fit = validate(anchors, e, im.w, coarse.cell);
      if (!fit)
        return;
      Geometry g;
      g.x = fit->x;
      g.cell = fit->cell;
      g.error = fit->error;
      g.offset = anchors[0].offset;
      int bad = anchors.size() == 1 ? 2 : 0;
      auto best = anchors[0];
      for (auto a : anchors) {
        bad += a.bad;
        if (a.bad < best.bad)
          best = a;
      }
      g.offset = best.offset;
      g.score = bad * 10 + fit->error - contrast(line, *fit, g.offset) / 255;
      g.row = centerY;
      g.y = sampleY;
      fits.push_back(g);
    };
    for (auto h : heads)
      for (auto t : tails)
        if (t.x > h.x)
          add({h, t});
    if (fits.empty()) {
      for (auto h : heads)
        add({h});
      for (auto t : tails)
        add({t});
    }
    for (auto g : fits) {
      if (g.x < -.5 || g.x + COLS * g.cell > im.w + .5)
        continue;
      auto dup = std::find_if(geometries.begin(), geometries.end(), [&](auto a) {
        return std::abs(a.x - g.x) < g.cell * .25 &&
               std::abs(a.x + TAIL * a.cell - g.x - TAIL * g.cell) < g.cell * .25;
      });
      if (dup == geometries.end())
        geometries.push_back(g);
      else if (g.score < dup->score)
        *dup = g;
    }
  }
  std::stable_sort(geometries.begin(), geometries.end(),
                   [](auto a, auto b) { return a.score < b.score; });
  if (geometries.empty() ||
      (geometries.size() > 1 && geometries[1].score - geometries[0].score < .05))
    return {};
  auto best = geometries[0];
  auto vertical = bounds(im, best, best.row);
  best.y = vertical.first;
  best.height = vertical.second;
  return best;
}
std::optional<pidan_watermark> scanBands(const Bytes &lum, int w, int h, const Image &large,
                                         double inv, int yOff) {
  if (w < 1 || h < 3)
    return {};
  struct Candidate {
    int y;
    Line line;
    double strength;
  };
  std::vector<Candidate> rows;
  int tile = std::max(24, w / 16);
  for (int y = h - 2; y >= 1; y -= 6) {
    if (cancelled()) return {};
    Line line(w);
    std::vector<int> tiles((w + tile - 1) / tile);
    int count = 0, supported = 0;
    for (int x = 0; x < w; x++)
      line[x] = (lum[(y - 1) * w + x] + lum[y * w + x] + lum[(y + 1) * w + x]) / 3.;
    for (int x = 1; x < w - 1; x++) {
      double dx = std::abs(line[x + 1] - line[x - 1]) / 2,
             dy = std::abs(int(lum[(y - 1) * w + x]) - lum[(y + 1) * w + x]);
      if (dx >= 12 && dy <= std::max(24., dx)) {
        tiles[x / tile]++;
        count++;
      }
    }
    for (int n : tiles)
      if (n >= 2)
        supported++;
    if (count >= 16 && supported >= 3)
      rows.push_back({y, std::move(line), supported + std::min(count, 120) / 120.});
  }
  std::vector<Geometry> candidates;
  for (const auto &row : rows) {
    auto fit = locateRow(row.line);
    if (!fit)
      continue;
    fit->y = row.y - fit->cell;
    fit->score -= row.strength * .001;
    auto dup = std::find_if(candidates.begin(), candidates.end(), [&](auto a) {
      return std::abs(a.x - fit->x) < fit->cell * .25 &&
             std::abs(a.cell - fit->cell) * TAIL < fit->cell * .25 &&
             std::abs(a.y - fit->y) <= fit->cell * 3.5;
    });
    if (dup == candidates.end())
      candidates.push_back(*fit);
    else if (fit->score < dup->score)
      *dup = *fit;
  }
  std::stable_sort(candidates.begin(), candidates.end(),
                   [](auto a, auto b) { return a.score < b.score; });
  if (candidates.size() > 16)
    candidates.resize(16);
  for (auto candidate : candidates) {
    if (cancelled()) return {};
    candidate.x *= inv;
    candidate.y = (candidate.y + yOff) * inv;
    candidate.cell *= inv;
    int top = std::max(0, int(std::floor(candidate.y - candidate.cell * 2))),
        height = std::min(large.h - top, int(std::ceil(candidate.cell * 7)));
    if (height <= 0)
      continue;
    Image roi = resize(large, 0, top, large.w, height, large.w, height);
    candidate.y -= top;
    auto loc = locateRegion(roi, candidate);
    if (!loc)
      continue;
    auto cells = sample(roi, *loc);
    if (!cells)
      continue;
    auto result = decode(*cells);
    if (result) {
      result->method = 2;
      return result;
    }
  }
  return {};
}
} // namespace pidan
