#include "ReactionDiffusion.h"

#include <algorithm>
#include <cstring>

namespace ReactionDiffusion {
namespace {

// Q16 helpers for writing the playground's decimal rates readably.
constexpr uint16_t q16(const double value) { return static_cast<uint16_t>(value * 65536.0 + 0.5); }

// The U-Skate World pair is taken from the playground with the mask loaded as
// the style map, which is the look this wallpaper is matching.
// TODO(rates): the remaining five still use the playground's default dA/dB with
// their own feed/kill, and the same styled offset U-Skate uses. Replace each
// line once its pair has been tuned against this mask.
constexpr Rates PLAYGROUND_STYLED = {q16(0.0316), q16(0.0482), q16(0.2097), q16(0.1050)};

constexpr Preset PRESETS[PRESET_COUNT] = {
    // U-Skate World
    {{q16(0.0620), q16(0.0609), q16(0.2286), q16(0.1295)}, PLAYGROUND_STYLED, 2000, ONE / 5},
    // Maze
    {{q16(0.0290), q16(0.0570), q16(0.2097), q16(0.1050)}, PLAYGROUND_STYLED, 2000, ONE / 5},
    // Worms
    {{q16(0.0780), q16(0.0610), q16(0.2097), q16(0.1050)}, PLAYGROUND_STYLED, 2000, ONE / 5},
    // Solitons
    {{q16(0.0300), q16(0.0620), q16(0.2097), q16(0.1050)}, PLAYGROUND_STYLED, 2000, ONE / 5},
    // Holes
    {{q16(0.0390), q16(0.0580), q16(0.2097), q16(0.1050)}, PLAYGROUND_STYLED, 2000, ONE / 5},
    // Chaos
    {{q16(0.0260), q16(0.0510), q16(0.2097), q16(0.1050)}, PLAYGROUND_STYLED, 2000, ONE / 5},
};

// Mixing constants shared with BitmapHelpers' value noise, so the firmware has
// one hash family rather than a second generator.
uint32_t mix(uint32_t value) {
  value = value * 374761393u + 668265263u;
  value = (value ^ (value >> 13)) * 1274126177u;
  return value ^ (value >> 16);
}

int16_t clampQ14(const int32_t value) { return static_cast<int16_t>(std::clamp<int32_t>(value, 0, ONE)); }

// The playground's kernel: 0.2 orthogonal, 0.05 diagonal, -1 centre, which is
// 4, 1 and -20 over 20.
constexpr int KERNEL_ORTHOGONAL = 4;
constexpr int KERNEL_DIAGONAL = 1;
constexpr int KERNEL_CENTRE = 20;
constexpr int KERNEL_SUM = 20;

int32_t kernelDivide(const int32_t weighted) {
  return weighted >= 0 ? (weighted + KERNEL_SUM / 2) / KERNEL_SUM : -((-weighted + KERNEL_SUM / 2) / KERNEL_SUM);
}

// Rounded Q16 multiply. Rounding matters: truncating here lets the small feed
// and kill terms fall to zero and the pattern dies out.
int32_t mulQ16(const int32_t value, const uint32_t rateQ16) {
  const int64_t scaled = static_cast<int64_t>(value) * rateQ16;
  return static_cast<int32_t>(scaled >= 0 ? (scaled + (1 << 15)) >> 16 : -((-scaled + (1 << 15)) >> 16));
}

}  // namespace

bool Field::styled(const int x, const int y) const {
  if (styleMap == nullptr) return false;
  return (styleMap[static_cast<size_t>(y) * styleRowBytes() + static_cast<size_t>(x >> 3)] >> (7 - (x & 7))) & 1u;
}

const Preset& preset(const size_t index) { return PRESETS[index < PRESET_COUNT ? index : 0]; }

void seed(const Field& field, const uint32_t randomSeed) {
  if (!field.valid()) return;

  // The playground seeds one circle and lets the front crawl outward, which is
  // why its counter reaches millions of iterations before the canvas fills.
  // Scattering the seed instead gives the same converged pattern in a few
  // thousand steps, which is the difference between this being feasible on the
  // device and not.
  for (size_t i = 0; i < field.cells(); ++i) {
    field.a[i] = static_cast<int16_t>(ONE);
    field.b[i] = 0;
  }

  for (int y = 0; y < field.height; ++y) {
    for (int x = 0; x < field.width; ++x) {
      const size_t index = static_cast<size_t>(y) * field.width + x;
      const uint32_t noise = mix(randomSeed ^ mix(static_cast<uint32_t>(index)));
      // A sparse scatter: dense enough that every region has a nucleus within a
      // short diffusion distance, sparse enough to stay irregular.
      if ((noise % 24u) != 0u) continue;
      field.b[index] = static_cast<int16_t>(ONE);
    }
  }
}

void advance(const Field& field, const Preset& settings, const int steps, int16_t* scratch) {
  if (!field.valid() || steps <= 0 || scratch == nullptr) return;

  const int w = field.width;
  const int h = field.height;

  // Three saved rows per species: the row above (already overwritten), the row
  // being written, and row 0, which the bottom row wraps onto after row 0 has
  // itself been overwritten.
  int16_t* aboveA = scratch;
  int16_t* aboveB = scratch + w;
  int16_t* currentA = scratch + 2 * w;
  int16_t* currentB = scratch + 3 * w;
  int16_t* firstA = scratch + 4 * w;
  int16_t* firstB = scratch + 5 * w;

  for (int step = 0; step < steps; ++step) {
    std::memcpy(firstA, field.a, static_cast<size_t>(w) * sizeof(int16_t));
    std::memcpy(firstB, field.b, static_cast<size_t>(w) * sizeof(int16_t));

    for (int y = 0; y < h; ++y) {
      int16_t* rowA = field.a + static_cast<size_t>(y) * w;
      int16_t* rowB = field.b + static_cast<size_t>(y) * w;
      std::memcpy(currentA, rowA, static_cast<size_t>(w) * sizeof(int16_t));
      std::memcpy(currentB, rowB, static_cast<size_t>(w) * sizeof(int16_t));

      // Wrap vertically. The row above is only still in the field for y == 0,
      // where the bottom row has not been touched yet this step.
      const int16_t* upA = (y == 0) ? field.a + static_cast<size_t>(h - 1) * w : aboveA;
      const int16_t* upB = (y == 0) ? field.b + static_cast<size_t>(h - 1) * w : aboveB;
      const int16_t* downA = (y == h - 1) ? firstA : rowA + w;
      const int16_t* downB = (y == h - 1) ? firstB : rowB + w;

      for (int x = 0; x < w; ++x) {
        const int left = (x == 0) ? w - 1 : x - 1;
        const int right = (x == w - 1) ? 0 : x + 1;

        const int32_t a = currentA[x];
        const int32_t b = currentB[x];

        const int32_t orthogonalA = upA[x] + downA[x] + currentA[left] + currentA[right];
        const int32_t diagonalA = upA[left] + upA[right] + downA[left] + downA[right];
        const int32_t orthogonalB = upB[x] + downB[x] + currentB[left] + currentB[right];
        const int32_t diagonalB = upB[left] + upB[right] + downB[left] + downB[right];

        const int32_t lapA =
            kernelDivide(KERNEL_ORTHOGONAL * orthogonalA + KERNEL_DIAGONAL * diagonalA - KERNEL_CENTRE * a);
        const int32_t lapB =
            kernelDivide(KERNEL_ORTHOGONAL * orthogonalB + KERNEL_DIAGONAL * diagonalB - KERNEL_CENTRE * b);

        // The style map picks the rates, which is what makes the mask visible.
        const Rates& rates = field.styled(x, y) ? settings.styled : settings.base;

        // a * b^2, both Q14, rounded at each step to stop a slow drift to zero.
        const int32_t ab = (a * b + (1 << 13)) >> 14;
        const int32_t abb = (ab * b + (1 << 13)) >> 14;

        rowA[x] = clampQ14(a + mulQ16(lapA, rates.diffuseA) - abb + mulQ16(ONE - a, rates.feed));
        rowB[x] = clampQ14(b + mulQ16(lapB, rates.diffuseB) + abb -
                           mulQ16(b, static_cast<uint32_t>(rates.feed) + rates.kill));
      }

      std::swap(aboveA, currentA);
      std::swap(aboveB, currentB);
    }
  }
}

int16_t sampleB(const Field& field, const int x, const int outWidth, const int y, const int outHeight) {
  if (!field.valid() || outWidth <= 0 || outHeight <= 0) return 0;

  // Q8 position inside the grid, sampling at pixel centres.
  const int32_t fx = static_cast<int32_t>(((2 * x + 1) * static_cast<int64_t>(field.width) * 256) / (2 * outWidth));
  const int32_t fy = static_cast<int32_t>(((2 * y + 1) * static_cast<int64_t>(field.height) * 256) / (2 * outHeight));
  const int32_t tx = fx & 0xFF;
  const int32_t ty = fy & 0xFF;

  const int x0 = std::clamp<int32_t>(fx >> 8, 0, field.width - 1);
  const int y0 = std::clamp<int32_t>(fy >> 8, 0, field.height - 1);
  const int x1 = std::min(x0 + 1, field.width - 1);
  const int y1 = std::min(y0 + 1, field.height - 1);

  const int32_t topLeft = field.b[static_cast<size_t>(y0) * field.width + x0];
  const int32_t topRight = field.b[static_cast<size_t>(y0) * field.width + x1];
  const int32_t bottomLeft = field.b[static_cast<size_t>(y1) * field.width + x0];
  const int32_t bottomRight = field.b[static_cast<size_t>(y1) * field.width + x1];

  const int32_t top = topLeft + (((topRight - topLeft) * tx) >> 8);
  const int32_t bottom = bottomLeft + (((bottomRight - bottomLeft) * tx) >> 8);
  return static_cast<int16_t>(top + (((bottom - top) * ty) >> 8));
}

Spread spreadOfB(const Field& field) {
  Spread spread;
  if (!field.valid()) return spread;

  const size_t count = field.cells();
  int64_t total = 0;
  for (size_t i = 0; i < count; ++i) total += field.b[i];
  spread.mean = static_cast<int32_t>(total / static_cast<int64_t>(count));

  uint64_t squared = 0;
  for (size_t i = 0; i < count; ++i) {
    const int64_t delta = field.b[i] - spread.mean;
    squared += static_cast<uint64_t>(delta * delta);
  }
  spread.variance = static_cast<uint32_t>(std::min<uint64_t>(squared / count, UINT32_MAX));
  return spread;
}

}  // namespace ReactionDiffusion
