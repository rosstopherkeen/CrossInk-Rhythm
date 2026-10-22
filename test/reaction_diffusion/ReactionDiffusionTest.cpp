#include <gtest/gtest.h>

#include <cstdlib>
#include <vector>

#include "ReactionDiffusion.h"

namespace {

using ReactionDiffusion::Field;
using ReactionDiffusion::ONE;
using ReactionDiffusion::Preset;

// The smallest grid that ships: the non-PSRAM profile in SleepArtGenerator.
// Presets are sensitive to grid size, so testing a smaller one proves little.
constexpr int W = 100;
constexpr int H = 200;
constexpr size_t STYLE_ROW = (W + 7) / 8;

struct Grid {
  std::vector<int16_t> a{std::vector<int16_t>(static_cast<size_t>(W) * H)};
  std::vector<int16_t> b{std::vector<int16_t>(static_cast<size_t>(W) * H)};
  std::vector<uint8_t> style{std::vector<uint8_t>(STYLE_ROW * H, 0)};
  std::vector<int16_t> scratch{std::vector<int16_t>(ReactionDiffusion::scratchCount(W))};

  // A centred block standing in for the real mask's letterforms.
  void styleBlock() {
    for (int y = H / 4; y < (3 * H) / 4; ++y) {
      for (int x = W / 4; x < (3 * W) / 4; ++x) style[y * STYLE_ROW + (x >> 3)] |= 0x80 >> (x & 7);
    }
  }

  Field field() { return Field{a.data(), b.data(), style.data(), W, H}; }
  Field fieldWithoutStyle() { return Field{a.data(), b.data(), nullptr, W, H}; }
};

size_t drawnPixels(const Grid& grid, const Preset& settings) {
  size_t count = 0;
  for (const int16_t value : grid.b) {
    if (value > settings.thresholdQ14) ++count;
  }
  return count;
}

}  // namespace

// The failure mode fixed-point Gray-Scott actually has is a quiet death: the
// field drifts to a constant and the sleep screen renders flat. Every preset
// has to still be varying after its own full step count.
TEST(ReactionDiffusion, EveryPresetStillHasStructureAfterItsFullRun) {
  for (size_t index = 0; index < ReactionDiffusion::PRESET_COUNT; ++index) {
    const Preset& settings = ReactionDiffusion::preset(index);
    Grid grid;
    grid.styleBlock();
    const Field field = grid.field();
    ReactionDiffusion::seed(field, 0x1234u + index);
    ReactionDiffusion::advance(field, settings, settings.steps, grid.scratch.data());

    EXPECT_GT(ReactionDiffusion::spreadOfB(field).variance, 1000u)
        << "preset " << index << " collapsed to a constant";

    const double fraction = static_cast<double>(drawnPixels(grid, settings)) / grid.b.size();
    EXPECT_GT(fraction, 0.05) << "preset " << index << " drew almost nothing";
    EXPECT_LT(fraction, 0.95) << "preset " << index << " filled the screen";
  }
}

// The style map is what makes the mask visible, so the two regions have to end
// up genuinely different. Without it the field must be uniform in character.
TEST(ReactionDiffusion, StyleMapMakesTheMaskedRegionDiffer) {
  const Preset& settings = ReactionDiffusion::preset(0);
  Grid grid;
  grid.styleBlock();
  const Field field = grid.field();
  ReactionDiffusion::seed(field, 42u);
  ReactionDiffusion::advance(field, settings, settings.steps, grid.scratch.data());

  size_t insideOn = 0, insideTotal = 0, outsideOn = 0, outsideTotal = 0;
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      const bool on = grid.b[static_cast<size_t>(y) * W + x] > settings.thresholdQ14;
      if (field.styled(x, y)) {
        ++insideTotal;
        insideOn += on ? 1 : 0;
      } else {
        ++outsideTotal;
        outsideOn += on ? 1 : 0;
      }
    }
  }
  ASSERT_GT(insideTotal, 0u);
  ASSERT_GT(outsideTotal, 0u);
  const double inside = static_cast<double>(insideOn) / insideTotal;
  const double outside = static_cast<double>(outsideOn) / outsideTotal;
  EXPECT_GT(std::abs(inside - outside), 0.1) << "inside " << inside << " outside " << outside;
}

// Time slicing is what keeps generation off the lock path, so splitting a run
// must be indistinguishable from doing it in one go.
TEST(ReactionDiffusion, SlicedRunMatchesSingleRun) {
  const Preset& settings = ReactionDiffusion::preset(0);
  Grid whole;
  Grid sliced;
  whole.styleBlock();
  sliced.styleBlock();
  ReactionDiffusion::seed(whole.field(), 99u);
  ReactionDiffusion::seed(sliced.field(), 99u);

  ReactionDiffusion::advance(whole.field(), settings, 300, whole.scratch.data());
  for (int done = 0; done < 300; done += 37) {
    ReactionDiffusion::advance(sliced.field(), settings, std::min(37, 300 - done), sliced.scratch.data());
  }
  EXPECT_EQ(sliced.a, whole.a);
  EXPECT_EQ(sliced.b, whole.b);
}

// Different seeds must give different images; that is the point of rotating the
// seed on each sleep.
TEST(ReactionDiffusion, DifferentSeedsDiverge) {
  const Preset& settings = ReactionDiffusion::preset(0);
  Grid first;
  Grid second;
  first.styleBlock();
  second.styleBlock();
  ReactionDiffusion::seed(first.field(), 1u);
  ReactionDiffusion::seed(second.field(), 2u);
  ReactionDiffusion::advance(first.field(), settings, 1000, first.scratch.data());
  ReactionDiffusion::advance(second.field(), settings, 1000, second.scratch.data());

  size_t differing = 0;
  for (size_t i = 0; i < first.b.size(); ++i) {
    if (std::abs(first.b[i] - second.b[i]) > ONE / 32) ++differing;
  }
  EXPECT_GT(differing, first.b.size() / 20);
}

// The upscale to the panel must stay inside the grid and track its values.
TEST(ReactionDiffusion, SampleStaysInRangeAcrossTheWholeOutput) {
  const Preset& settings = ReactionDiffusion::preset(0);
  Grid grid;
  grid.styleBlock();
  const Field field = grid.field();
  ReactionDiffusion::seed(field, 3u);
  ReactionDiffusion::advance(field, settings, 500, grid.scratch.data());

  constexpr int OUT_W = 480;
  constexpr int OUT_H = 800;
  int32_t lowest = ONE;
  int32_t highest = 0;
  for (int y = 0; y < OUT_H; ++y) {
    for (int x = 0; x < OUT_W; ++x) {
      const int16_t value = ReactionDiffusion::sampleB(field, x, OUT_W, y, OUT_H);
      ASSERT_GE(value, 0);
      ASSERT_LE(value, ONE);
      lowest = std::min<int32_t>(lowest, value);
      highest = std::max<int32_t>(highest, value);
    }
  }
  // A flat sample would mean the upscale lost the pattern.
  EXPECT_GT(highest - lowest, ONE / 10);
}
