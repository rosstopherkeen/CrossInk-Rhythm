#pragma once

#include <cstddef>
#include <cstdint>

// Gray-Scott reaction-diffusion, fixed point, for the Reaction sleep screen.
//
// Follows the reaction-diffusion playground
// (https://jasonwebb.github.io/reaction-diffusion-playground/app.html):
//
//   A' = A + (dA * lapA - A*B^2 + f*(1 - A)) * dt
//   B' = B + (dB * lapB + A*B^2 - (k + f)*B) * dt
//
// with the playground's 3x3 Laplacian (0.2 orthogonal, 0.05 diagonal, -1
// centre) and dt = 1.
//
// The mask is a *style map*, not a seed: cells inside it use a second set of
// rates, so the letterforms show up as a region of different pattern character.
// Seeding is a plain circle in the middle, as in the playground.
//
// Pure math with no storage, renderer or settings access so the host tests can
// run it; SleepArtGenerator drives it on the device.
//
// State is Q14 (ONE == 1.0) in int16_t. Q12 was not enough: the f * (1 - A)
// term truncates to zero near A == 1 and the pattern dies instead of settling.
// Rates are Q16 because useful presets sit about 0.001 apart.
namespace ReactionDiffusion {

constexpr int32_t ONE = 1 << 14;

// One set of Gray-Scott rates, all Q16.
struct Rates {
  uint16_t feed;
  uint16_t kill;
  uint16_t diffuseA;
  uint16_t diffuseB;
};

struct Preset {
  Rates base;            // outside the style map
  Rates styled;          // inside the style map
  uint16_t steps;        // iterations before the pattern is read out
  int16_t thresholdQ14;  // B above this is drawn
};

// Index order must match CrossPointSettings::SLEEP_SIMULATION minus SIM_RANDOM.
constexpr size_t PRESET_COUNT = 6;
const Preset& preset(size_t index);

// Grid state; the caller owns both planes, each width * height int16_t, plus
// the packed style map, one bit per cell, rows of (width + 7) / 8 bytes.
struct Field {
  int16_t* a = nullptr;
  int16_t* b = nullptr;
  const uint8_t* styleMap = nullptr;
  int width = 0;
  int height = 0;

  bool valid() const { return a != nullptr && b != nullptr && width > 1 && height > 1; }
  size_t cells() const { return static_cast<size_t>(width) * static_cast<size_t>(height); }
  size_t styleRowBytes() const { return static_cast<size_t>(width + 7) / 8u; }
  bool styled(int x, int y) const;
};

// Fills A, clears B, then seeds a circle of B in the middle. randomSeed jitters
// the circle's centre and radius so each run differs.
void seed(const Field& field, uint32_t randomSeed);

// Scratch holds the original values the single-buffered update still needs.
constexpr size_t scratchCount(const int width) { return static_cast<size_t>(width) * 6u; }

// Advances the field in place. Safe to call in slices: the state after
// advance(n) then advance(m) matches advance(n + m).
void advance(const Field& field, const Preset& settings, int steps, int16_t* scratch);

// Bilinear sample of B for an output pixel, so the upscale to the screen is
// smooth rather than blocky when the grid is smaller than the panel.
int16_t sampleB(const Field& field, int x, int outWidth, int y, int outHeight);

// Spread of B across the grid. A pattern that has died collapses towards a
// constant, so the host test uses this as its liveness check.
struct Spread {
  int32_t mean = 0;
  uint32_t variance = 0;
};
Spread spreadOfB(const Field& field);

}  // namespace ReactionDiffusion
