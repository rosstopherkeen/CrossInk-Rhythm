#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "DailyMinutesLog.h"
#include "FinishedBooksIndex.h"
#include "activities/reader/GlobalReadingStats.h"

class GfxRenderer;
class MappedInputManager;

// Reading Rhythm and Books Finished pages of the Reading Stats screen.
//
// Ported from XPoint (https://github.com/Belphemur/XPoint, MIT):
// BookStatsView.cpp renderReadingRhythmPage() / renderFinishedBooksPage().
// Differences from XPoint:
//   - cards use the Reading Stats header, insets and card metrics
//     (statsPageFrame) and stretch to fill the screen instead of a fixed size.
//   - read days come from GlobalReadingStats::readingHistoryBits; minutes come
//     from the DailyMinutesLog sidecar instead of global_stats.bin v6.
//   - estimated days (one-time backfill) are drawn lighter: outlined dots, a
//     light bar segment and a "~" prefix on the last-7-days values.
//   - the finished list fills the available height; it pages only when the
//     books do not fit.
namespace RhythmStatsView {

void renderReadingRhythmPage(GfxRenderer& renderer, const MappedInputManager* mappedInput,
                             const GlobalReadingStats& stats, const DailyMinutesLog& minutes, bool showMoreButton);

// First entry index of each list page, from the same row metrics the renderer
// uses. Always has at least one element.
std::vector<uint16_t> paginateFinishedBooks(const GfxRenderer& renderer, const MappedInputManager* mappedInput,
                                            const std::vector<FinishedBookEntry>& finishedBooks);

void renderFinishedBooksPage(GfxRenderer& renderer, const MappedInputManager* mappedInput,
                             const std::vector<FinishedBookEntry>& finishedBooks, uint32_t completedBooks,
                             const std::vector<uint16_t>& pageStarts, size_t listPage);

}  // namespace RhythmStatsView
