// Ported from packages/watermark-recognizer/src/scanWatermark.ts / watermarkPadding.ts; TS remains
// the regression oracle.
#include "internal.hpp"
#include <cstdlib>
#include <cstring>
namespace pidan {
// Pixel-center bilinear resampling; unlike platform canvas, behavior is identical on every target.
struct PixelView {
  const uint8_t *data;
  int w, h;
  int channels = 4;
  uint8_t channel(uint64_t offset) const {
    if (channels == 3) return data[(offset / 4) * 3 + offset % 4];
    unsigned alpha = data[(offset & ~uint64_t(3)) + 3];
    return uint8_t((data[offset] * alpha + 255 * (255 - alpha) + 127) / 255);
  }
};
static Image resizeView(const PixelView &im, int sx, int sy, int sw, int sh, int ow, int oh) {
  Image out(ow, oh);
  if (sw == ow && sh == oh) {
    for (int y = 0; y < oh; ++y) for (int x = 0; x < ow; ++x) {
      if (x == 0 && cancelled()) return {};
      uint64_t input = (uint64_t(sy + y) * im.w + sx + x) * 4;
      size_t output = (size_t(y) * ow + x) * 4;
      for (int c = 0; c < 3; ++c) out.data[output + c] = im.channel(input + c);
    }
    return out;
  }
  for (int y = 0; y < oh; y++) {
    if (cancelled()) return {};
    double fy = sy + (y + .5) * sh / oh - .5;
    int y0 = clamp(int(std::floor(fy)), sy, sy + sh - 1),
        y1 = clamp(int(std::floor(fy)) + 1, sy, sy + sh - 1);
    double ty = fy - std::floor(fy);
    for (int x = 0; x < ow; x++) {
      double fx = sx + (x + .5) * sw / ow - .5;
      int x0 = clamp(int(std::floor(fx)), sx, sx + sw - 1),
          x1 = clamp(int(std::floor(fx)) + 1, sx, sx + sw - 1);
      double tx = fx - std::floor(fx);
      for (int c = 0; c < 3; c++) {
        double a = im.channel((uint64_t(y0) * im.w + x0) * 4 + c) * (1 - tx) +
                   im.channel((uint64_t(y0) * im.w + x1) * 4 + c) * tx,
               b = im.channel((uint64_t(y1) * im.w + x0) * 4 + c) * (1 - tx) +
                   im.channel((uint64_t(y1) * im.w + x1) * 4 + c) * tx;
        out.data[(size_t(y) * ow + x) * 4 + c] = uint8_t(clamp(rnd(a * (1 - ty) + b * ty), 0, 255));
      }
    }
  }
  return out;
}
Image resize(const Image &im, int sx, int sy, int sw, int sh, int ow, int oh) {
  return resizeView({im.data.data(), im.w, im.h}, sx, sy, sw, sh, ow, oh);
}
static int paddingProbe(const Image &p, int originalWidth) {
  int w = p.w, h = p.h;
  if (w < 32 || h < 16)
    return 0;
  std::vector<RGB> corners;
  for (int y0 : {0, h - 2})
    for (int x0 : {0, w - 2})
      for (int dy = 0; dy < 2; dy++)
        for (int dx = 0; dx < 2; dx++) {
          size_t k = (size_t(y0 + dy) * w + x0 + dx) * 4;
          RGB rgb = {double(p.data[k]), double(p.data[k + 1]), double(p.data[k + 2])};
          if (std::min({rgb[0], rgb[1], rgb[2]}) < 235)
            return 0;
          corners.push_back(rgb);
        }
  RGB bg{};
  for (int c = 0; c < 3; c++) {
    Line values;
    for (auto rgb : corners)
      values.push_back(rgb[c]);
    std::sort(values.begin(), values.end());
    bg[c] = (values[7] + values[8]) / 2;
  }
  for (auto rgb : corners)
    for (int c = 0; c < 3; c++)
      if (std::abs(rgb[c] - bg[c]) > 8)
        return 0;
  if (*std::max_element(bg.begin(), bg.end()) - *std::min_element(bg.begin(), bg.end()) > 12)
    return 0;
  auto blank = [&](int x) {
    for (int y = 0; y < h; y++)
      for (int c = 0; c < 3; c++)
        if (std::abs(p.data[(size_t(y) * w + x) * 4 + c] - bg[c]) > 8)
          return false;
    return true;
  };
  int left = 0, right = 0;
  while (left < w && blank(left))
    left++;
  if (left == w)
    return 0;
  while (right < w && blank(w - 1 - right))
    right++;
  int b = std::min(left, right), content = w - 2 * b,
      guard = std::min(b, std::max({4, int(std::ceil(w * .03)), int(std::ceil(b * .05)),
                                    int(std::ceil((std::ceil(w * .2) - content) / 2))}));
  return int(std::floor((b - guard) * double(originalWidth) / w));
}
static void probeSize(int w, int h, int &pw, int &ph) {
  double scale = std::min({1., double(PIDAN_PROBE_MAX_SIDE) / w, double(PIDAN_PROBE_MAX_SIDE) / h});
  pw = std::max(1, rnd(w * scale)); ph = std::max(1, rnd(h * scale));
}
static void imagePlan(int w, int h, int inset, int *plan) {
  int cropped = w - 2 * inset;
  bool bottom = h > cropped && h > 2000;
  int sh = bottom ? cropped : h;
  double scale = std::min(1., 2000. / std::max(cropped, sh));
  plan[0] = inset; plan[1] = bottom ? h - cropped : 0;
  plan[2] = cropped; plan[3] = sh;
  plan[4] = std::max(1, rnd(cropped * scale));
  plan[5] = std::max(1, rnd(sh * scale));
}
static std::optional<pidan_watermark> recognizeWork(const Image &large) {
  double k = 800. / large.w;
  int sh = std::max(1, rnd(large.h * k)), ch = std::min(800, sh),
      yOff = large.h > large.w ? sh - ch : 0;
  // Avoid allocating a very tall intermediate when scaling portrait images.
  Image small(800, ch);
  for (int y = 0; y < ch; y++) {
    if (cancelled()) return {};
    double fy = (y + yOff + .5) / k - .5;
    int y0 = clamp(int(std::floor(fy)), 0, large.h - 1),
        y1 = clamp(int(std::floor(fy)) + 1, 0, large.h - 1);
    double ty = fy - std::floor(fy);
    for (int x = 0; x < 800; x++) {
      double fx = (x + .5) / k - .5;
      int x0 = clamp(int(std::floor(fx)), 0, large.w - 1),
          x1 = clamp(int(std::floor(fx)) + 1, 0, large.w - 1);
      double tx = fx - std::floor(fx);
      for (int c = 0; c < 3; c++) {
        double a = large.data[(size_t(y0) * large.w + x0) * 4 + c] * (1 - tx) +
                   large.data[(size_t(y0) * large.w + x1) * 4 + c] * tx,
               b = large.data[(size_t(y1) * large.w + x0) * 4 + c] * (1 - tx) +
                   large.data[(size_t(y1) * large.w + x1) * 4 + c] * tx;
        small.data[(size_t(y) * 800 + x) * 4 + c] =
            uint8_t(clamp(rnd(a * (1 - ty) + b * ty), 0, 255));
      }
    }
  }
  Bytes lum(size_t(small.w) * small.h);
  for (size_t i = 0; i < lum.size(); i++)
    lum[i] =
        (77 * small.data[i * 4] + 150 * small.data[i * 4 + 1] + 29 * small.data[i * 4 + 2]) >> 8;
  if (cancelled()) return {};
  auto result = fast(lum, small.w, small.h);
  if (result)
    return result;
  if (cancelled()) return {};
  result = scanY(small, large, large.w / 800., yOff);
  if (result)
    return result;
  if (cancelled()) return {};
  return scanBands(lum, small.w, small.h, large, large.w / 800., yOff);
}
} // namespace pidan
extern "C" {
void *pidan_alloc(size_t length) {
  return std::malloc(length);
}
void pidan_free(void *pointer) {
  std::free(pointer);
}
const char *pidan_core_version(void) {
  return "0.2.1";
}
static int recognizePixels(const uint8_t *pixels, size_t length, int width, int height,
                         pidan_watermark *out, int channels) {
  if (!out)
    return 1;
  std::memset(out, 0, sizeof(*out));
  if (!pixels || width <= 0 || height <= 0 || uint64_t(width) * height > SIZE_MAX / channels ||
      uint64_t(width) * height * channels != length)
    return 1;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
  try {
#endif
    pidan::PixelView source{pixels, width, height, channels};
    int pw, ph, plan[6];
    pidan::probeSize(width, height, pw, ph);
    auto probe = pidan::resizeView(source, 0, 0, width, height, pw, ph);
    if (pidan::cancelled()) return 6;
    pidan::imagePlan(width, height, pidan::paddingProbe(probe, width), plan);
    auto work = pidan::resizeView(source, plan[0], plan[1], plan[2], plan[3], plan[4], plan[5]);
    if (pidan::cancelled()) return 6;
    auto result = pidan::recognizeWork(work);
    if (pidan::cancelled()) return 6;
    if (!result)
      return 2;
    *out = *result;
    return 0;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
  } catch (...) {
    return 5;
  }
#endif
}
int pidan_recognize_rgba(const uint8_t *p, size_t n, int w, int h, pidan_watermark *out) {
  return recognizePixels(p,n,w,h,out,4);
}
int pidan_recognize_rgb(const uint8_t *p, size_t n, int w, int h, pidan_watermark *out) {
  return recognizePixels(p,n,w,h,out,3);
}
int pidan_probe_size(int width, int height, int *size) {
  if (!size || width <= 0 || height <= 0) return 1;
  pidan::probeSize(width, height, size[0], size[1]);
  return 0;
}
int pidan_image_plan(const uint8_t *pixels, size_t length, int width, int height, int *plan) {
  if (!plan || !pixels || width <= 0 || height <= 0) return 1;
  int pw, ph;
  pidan::probeSize(width, height, pw, ph);
  if (length != size_t(pw) * ph * 4) return 1;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
  try {
#endif
    auto probe = pidan::resizeView({pixels, pw, ph}, 0, 0, pw, ph, pw, ph);
    if (pidan::cancelled()) return 6;
    pidan::imagePlan(width, height, pidan::paddingProbe(probe, width), plan);
    return 0;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
  } catch (...) { return 5; }
#endif
}
int pidan_recognize_work_rgba(const uint8_t *pixels, size_t length, int width, int height,
                              pidan_watermark *out) {
  if (!out) return 1;
  std::memset(out, 0, sizeof(*out));
  if (!pixels || width < 1 || height < 1 || width > 2000 || height > 2000 ||
      length != size_t(width) * height * 4) return 1;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
  try {
#endif
    auto work = pidan::resizeView({pixels, width, height}, 0, 0, width, height, width, height);
    if (pidan::cancelled()) return 6;
    auto result = pidan::recognizeWork(work);
    if (pidan::cancelled()) return 6;
    if (!result) return 2;
    *out = *result;
    return 0;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
  } catch (...) { return 5; }
#endif
}
static int hex_digit(char c) {
  // Uppercase only: the query contract fixes the exact two-character form.
  return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

int pidan_decode_cells(const char *hex, size_t length, int rows, int cols, uint16_t *out,
                       size_t capacity) {
  if (!hex || !out || rows < 1 || rows > 104 || cols < 1 || cols > 104)
    return 3;
  const size_t count = size_t(rows) * cols;
  if (length != count * 2)
    return 3;
  if (capacity < count)
    return 4;
  // Validate the complete grid before writing output.
  for (size_t i = 0; i < count; i++) {
    int high = hex_digit(hex[2 * i]), low = hex_digit(hex[2 * i + 1]);
    if (high < 0 || low < 0)
      return 3;
    const char *code = pidan_color_code(unsigned(high * 16 + low));
    if (!code)
      return 3;
  }
  for (size_t i = 0; i < count; i++)
    out[i] = uint16_t(hex_digit(hex[2 * i]) * 16 + hex_digit(hex[2 * i + 1]));
  return 0;
}
}
