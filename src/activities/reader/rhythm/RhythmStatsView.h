#pragma once

#include <cstddef>
#include <vector>

#include "DailyMinutesLog.h"
#include "FinishedBooksIndex.h"
#include "activities/reader/GlobalReadingStats.h"

class GfxRenderer;
class MappedInputManager;

// Renderers for the Reading Rhythm and Books Finished screens.
//
// Ported from XPoint (https://github.com/Belphemur/XPoint, MIT):
// BookStatsView.cpp renderReadingRhythmPage() / renderFinishedBooksPage().
// Differences from XPoint:
//   - read days come from GlobalReadingStats::readingHistoryBits; minutes come
//     from the DailyMinutesLog sidecar instead of global_stats.bin v6.
//   - days whose minutes are estimated (one-time backfill) are drawn lighter:
//     outlined dots, a dithered light segment on weekly bars and a "~" prefix
//     on the last-7-days values.
//   - CrossInk compact header (with touch back button on touch devices).
namespace RhythmStatsView {

constexpr size_t FINISHED_BOOKS_ENTRIES_PER_PAGE = 4;

void renderReadingRhythmPage(GfxRenderer& renderer, const MappedInputManager& mappedInput,
                             const GlobalReadingStats& stats, const DailyMinutesLog& minutes);

void renderFinishedBooksPage(GfxRenderer& renderer, const MappedInputManager& mappedInput,
                             const std::vector<FinishedBookEntry>& finishedBooks, const GlobalReadingStats& globalStats,
                             size_t pageIndex, size_t pageCount);

}  // namespace RhythmStatsView
