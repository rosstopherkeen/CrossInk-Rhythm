#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "BookRhythm.h"
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
//   - the finished list is a vertical scroll panel with a scroll bar; rows
//     open that book's stats.
namespace RhythmStatsView {

void renderReadingRhythmPage(GfxRenderer& renderer, const MappedInputManager* mappedInput,
                             const GlobalReadingStats& stats, const DailyMinutesLog& minutes, bool showMoreButton);

// Touch ids for finished-book rows: FINISHED_ROW_TOUCH_BASE + entry index.
constexpr int FINISHED_ROW_TOUCH_BASE = 100;

// Books that fit in the list panel when scrolled to `top` (at least 1 when any).
size_t finishedVisibleCount(const GfxRenderer& renderer, const MappedInputManager* mappedInput,
                            const std::vector<FinishedBookEntry>& finishedBooks, size_t top);
// Largest useful scroll position: the first top that shows the last book.
size_t finishedMaxTop(const GfxRenderer& renderer, const MappedInputManager* mappedInput,
                      const std::vector<FinishedBookEntry>& finishedBooks);

// Totals card plus a scrollable list; `selected` is outlined (-1 for none).
void renderFinishedBooksPage(GfxRenderer& renderer, const MappedInputManager* mappedInput,
                             const std::vector<FinishedBookEntry>& finishedBooks, uint32_t completedBooks, size_t top,
                             int selected);

// Book detail page: reading days over the book's start-to-finish span, weekly
// time within it, and a summary (days read, average per day, longest streak).
void renderBookRhythmPage(GfxRenderer& renderer, const MappedInputManager* mappedInput, const std::string& bookTitle,
                          const BookRhythm& rhythm);

}  // namespace RhythmStatsView
