#pragma once

#include <string>

#include "BookReadingStats.h"
#include "GlobalReadingStats.h"

class GfxRenderer;
class MappedInputManager;

namespace BookStatsTouchTarget {
constexpr int StartedDaysStat = 1;
constexpr int DateFieldBase = 10;
constexpr int DateFieldCount = 6;
constexpr int DateAdjustUp = 20;
constexpr int DateAdjustDown = 21;
constexpr int DateSave = 22;
constexpr int DateCancel = 23;

constexpr int dateField(const int index) { return DateFieldBase + index; }
}  // namespace BookStatsTouchTarget

void renderPerBookStatsPage(GfxRenderer& renderer, const MappedInputManager* mappedInput, const std::string& bookTitle,
                            const BookReadingStats& stats, float progressPercent, bool hasEstimatedTimeLeft,
                            uint32_t estimatedTimeLeftSeconds, bool showButtonHints, bool showEditButton,
                            bool showMoreButton);

void renderGlobalStatsPage(GfxRenderer& renderer, const MappedInputManager* mappedInput, const char* screenTitle,
                           const GlobalReadingStats& stats, bool showButtonHints, bool showMoreButton);

void renderNoRtcCombinedStatsPage(GfxRenderer& renderer, const MappedInputManager* mappedInput,
                                  const std::string& bookTitle, const BookReadingStats& bookStats,
                                  float progressPercent, bool hasEstimatedTimeLeft, uint32_t estimatedTimeLeftSeconds,
                                  const GlobalReadingStats& deviceStats, const GlobalReadingStats* allDevicesStats,
                                  bool showButtonHints);

void renderEditBookDatesPage(GfxRenderer& renderer, const MappedInputManager* mappedInput, const std::string& bookTitle,
                             const BookReadingStats& stats, int selectedField, bool showButtonHints);

// Shared frame for the Reading Rhythm and Books Finished pages so they use the
// same header, insets and card metrics as the pages above.
struct StatsPageFrame {
  int cardX = 0;
  int cardW = 0;
  int top = 0;     // first content row below the header
  int bottom = 0;  // last content row above button hints / page dots
  int cardGap = 0;
  int titleH = 0;  // card title band height
};

// Content area for a stats page, using the same layout choice (default or
// compact by minContentHeight) as the pages above. Does not draw.
StatsPageFrame statsPageFrame(const GfxRenderer& renderer, const MappedInputManager* mappedInput, bool showButtonHints,
                              int minContentHeight, int cardCount);
// Clears the screen and draws the stats header (touch back button on touch devices).
void drawStatsPageHeader(GfxRenderer& renderer, const MappedInputManager* mappedInput, const char* title);
void drawStatsCardFrame(const GfxRenderer& renderer, int x, int y, int w, int h, const char* title, int titleH);
void drawStatsValueCell(const GfxRenderer& renderer, int x, int w, int y, int h, const char* value, const char* label);
