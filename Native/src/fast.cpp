// Ported from packages/watermark-recognizer/src/watermarkFast.ts; TS remains the regression oracle.
#include "internal.hpp"
namespace pidan {
struct Run {
  double x, end;
  bool dark;
};
static std::optional<double> threshold(const Bytes &line) {
  std::array<int, 256> bins{};
  double sum = 0;
  int low = 255, high = 0;
  for (int v : line) {
    bins[v]++;
    sum += v;
    low = std::min(low, v);
    high = std::max(high, v);
  }
  if (high - low < 35)
    return {};
  double left = 0, leftSum = 0, best = -1;
  int cut = low;
  for (int v = low; v < high; v++) {
    left += bins[v];
    leftSum += bins[v] * v;
    double right = line.size() - left;
    if (!left || !right)
      continue;
    double score = left * right * sq(leftSum / left - (sum - leftSum) / right);
    if (score > best) {
      best = score;
      cut = v;
    }
  }
  double darkSum = 0, darkCount = 0;
  for (int v = low; v <= cut; v++) {
    darkSum += v * bins[v];
    darkCount += bins[v];
  }
  return (darkSum / darkCount + (sum - darkSum) / (line.size() - darkCount)) / 2;
}
static std::vector<Anchor> runAnchors(const std::vector<Run> &runs, const std::array<int, 8> &mark,
                                      bool reverse) {
  std::vector<int> widths;
  for (int i = 0; i < 8; i++) {
    if (i && mark[i] == mark[i - 1])
      widths.back()++;
    else
      widths.push_back(1);
  }
  int n = int(widths.size());
  std::vector<Anchor> found;
  for (int step = 0; step <= int(runs.size()) - n; step++) {
    int i = reverse ? int(runs.size()) - n - step : step;
    if (runs[i].dark != (mark[0] == 2))
      continue;
    std::vector<Point> points;
    int k = 0;
    for (int j = 0; j < n - 1; j++) {
      k += widths[j];
      points.push_back({double(k), runs[i + j].end});
    }
    auto fit = fitLine(points, 2.85);
    if (!fit)
      continue;
    double error = 0, maxError = 0;
    for (auto p : points) {
      double v = std::abs(p.x - fit->x - p.k * fit->cell) / fit->cell;
      error += v * v;
      maxError = std::max(maxError, v);
    }
    if (maxError > .3 || runs[i].end - runs[i].x < fit->cell * widths[0] * .6 ||
        runs[i + n - 1].end - runs[i + n - 1].x < fit->cell * widths.back() * .6)
      continue;
    bool duplicate = false;
    for (auto a : found)
      if (std::abs(a.x - fit->x) < fit->cell * .3 && std::abs(a.cell - fit->cell) < fit->cell * .05)
        duplicate = true;
    if (!duplicate) {
      Anchor a;
      a.x = fit->x;
      a.cell = fit->cell;
      a.error = error / points.size();
      found.push_back(a);
      std::stable_sort(found.begin(), found.end(),
                       [](auto a, auto b) { return a.error < b.error; });
      if (found.size() > 3)
        found.pop_back();
    }
  }
  return found;
}
std::optional<pidan_watermark> fast(const Bytes &lum, int w, int h) {
  if (w < 1 || h < 3)
    return {};
  int last = h - 2, middle = last - rnd((last - std::floor(h / 2.)) / 6) * 6;
  for (int direction : {1, -1}) {
    std::set<std::string> previous;
    for (int y = direction == 1 ? middle : middle - 6; y >= 1 && y <= last; y += direction * 6) {
    if (cancelled()) return {};
      Bytes line(lum.begin() + y * w, lum.begin() + (y + 1) * w);
      auto cut = threshold(line);
      std::set<std::string> seen;
      if (!cut) {
        previous = seen;
        continue;
      }
      std::vector<Run> runs;
      double x = 0;
      bool dark = line[0] < *cut;
      for (int i = 1; i < w; i++) {
        bool next = line[i] < *cut;
        if (next == dark)
          continue;
        double edge = i - .5 + (*cut - line[i - 1]) / (double(line[i]) - line[i - 1]);
        runs.push_back({x, edge, dark});
        x = edge;
        dark = next;
      }
      runs.push_back({x, double(w), dark});
      auto heads = runAnchors(runs, HEAD, false), tails = runAnchors(runs, END, true);
      std::vector<Fit> pairs;
      for (auto head : heads)
        for (auto tail : tails) {
          double cell = (tail.x - head.x) / TAIL;
          if (cell < 2.85 || head.x < -.5 || head.x + COLS * cell > w + .5 ||
              std::abs(head.cell - cell) > cell * .12 || std::abs(tail.cell - cell) > cell * .12)
            continue;
          pairs.push_back({head.x, cell,
                           head.error + tail.error + std::abs(head.cell - cell) / cell +
                               std::abs(tail.cell - cell) / cell});
        }
      std::stable_sort(pairs.begin(), pairs.end(),
                       [](auto a, auto b) { return a.error < b.error; });
      int attempts = 0;
      for (auto pair : pairs) {
    if (cancelled()) return {};
        if (attempts >= 3)
          break;
        std::array<int, COLS> cells{};
        for (int k = 0; k < COLS; k++) {
          int start = std::max(0, int(std::ceil(pair.x + (k + .25) * pair.cell - .5))),
              end = std::min(w - 1, int(std::floor(pair.x + (k + .75) * pair.cell - .5)));
          double sum = 0;
          int count = 0;
          for (int p = start; p <= end; p++) {
            sum += line[p];
            count++;
          }
          double v =
              count ? sum / count : line[clamp(rnd(pair.x + (k + .5) * pair.cell - .5), 0, w - 1)];
          cells[k] = v < *cut ? 2 : 1;
        }
        bool match = true;
        for (int k = 0; k < 8; k++)
          if (cells[k] != HEAD[k] || cells[TAIL + k] != END[k])
            match = false;
        if (!match)
          continue;
        std::string key =
            std::to_string(rnd(pair.x)) + ":" + std::to_string(rnd(pair.cell * 100)) + ":";
        for (int c : cells)
          key += char('0' + c);
        seen.insert(key);
        if (previous.count(key))
          continue;
        attempts++;
        auto result = decode(cells);
        if (result) {
          result->method = 1;
          return result;
        }
      }
      previous = std::move(seen);
    }
  }
  return {};
}
} // namespace pidan
