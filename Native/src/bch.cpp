// Ported from packages/watermark-recognizer/src/bchCodec.ts / watermarkBits.ts; TS remains the
// regression oracle.
#include "internal.hpp"
namespace pidan {
struct Field {
  std::array<int, 254> exp{};
  std::array<int, 128> log{};
  Field() {
    int v = 1;
    for (int i = 0; i < 127; i++) {
      exp[i] = v;
      log[v] = i;
      v <<= 1;
      if (v & 128)
        v ^= 0x83;
    }
    for (int i = 127; i < 254; i++)
      exp[i] = exp[i - 127];
  }
  int mul(int a, int b) const {
    return a && b ? exp[log[a] + log[b]] : 0;
  }
  int div(int a, int b) const {
    return a ? exp[(log[a] + 127 - log[b]) % 127] : 0;
  }
};
std::optional<pidan_watermark> decode(const std::array<int, COLS> &cells) {
  static const Field f;
  std::array<int, 104> received{};
  for (int i = 0; i < COLS; i++)
    if (cells[i] != 1 && cells[i] != 2)
      return {};
  for (int i = 0; i < 104; i++)
    received[i] = cells[i + 8] - 1;
  auto syndromes = [&]() {
    std::array<int, 18> s{};
    for (int i = 0; i < 104; i++)
      if (received[i])
        for (int j = 1; j <= 18; j++)
          s[j - 1] ^= f.exp[((103 - i) * j) % 127];
    return s;
  };
  auto s = syndromes();
  if (std::any_of(s.begin(), s.end(), [](int v) { return v != 0; })) {
    std::array<int, 19> loc{}, prev{};
    loc[0] = prev[0] = 1;
    int degree = 0, distance = 1, last = 1;
    for (int step = 0; step < 18; step++) {
      int discrepancy = s[step];
      for (int i = 1; i <= degree; i++)
        discrepancy ^= f.mul(loc[i], s[step - i]);
      if (!discrepancy) {
        distance++;
        continue;
      }
      auto before = loc;
      int factor = f.div(discrepancy, last);
      for (int i = 0; i + distance < 19; i++)
        loc[i + distance] ^= f.mul(factor, prev[i]);
      if (2 * degree <= step) {
        degree = step + 1 - degree;
        prev = before;
        last = discrepancy;
        distance = 1;
      } else
        distance++;
    }
    if (degree < 1 || degree > 9)
      return {};
    std::vector<int> errors;
    for (int power = 0; power < 127; power++) {
      int z = f.exp[(127 - power) % 127], v = loc[degree];
      for (int j = degree - 1; j >= 0; j--)
        v = f.mul(v, z) ^ loc[j];
      if (v)
        continue;
      if (power >= 104)
        return {};
      errors.push_back(103 - power);
    }
    if (int(errors.size()) != degree)
      return {};
    for (int p : errors)
      received[p] ^= 1;
    auto corrected = syndromes();
    if (std::any_of(corrected.begin(), corrected.end(), [](int v) { return v != 0; }))
      return {};
  }
  if (received[2] || received[3])
    return {};
  pidan_watermark result{};
  result.mirror = received[0];
  result.watermark = received[1];
  for (int i = 0; i < 44; i++)
    result.share_id_bits[i] = char('0' + received[i + 4]);
  return result;
}
} // namespace pidan
