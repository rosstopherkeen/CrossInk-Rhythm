#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "BookDailyMinutes.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/ReadingStatsUtils.h"

// Day-by-day reading for one book, for the book detail "Book Rhythm" page.
//
// Window: the book's start date to its finish date (or today while ongoing).
// Days with real per-book minutes (BookDailyMinutes) use them. Days before the
// first real recording are estimated: the book's remaining time is spread
// evenly over k = min(sessions, readDays) of the global read days in the
// window (DailyMinutesBackfill::spreadEvenly), so they are only approximate
// when two books were read in the same weeks.
struct BookRhythm {
  static constexpr int MAX_WEEKS = 13;

  struct Day {
    uint16_t minutes = 0;
    bool estimated = false;
  };

  uint32_t startDay = 0;       // window start (day index)
  uint32_t endDay = 0;         // window end, inclusive
  uint32_t firstShownDay = 0;  // first day of the first displayed week
  int weekCount = 0;           // displayed weeks, 1..MAX_WEEKS
  bool truncated = false;      // window longer than MAX_WEEKS weeks
  std::array<Day, MAX_WEEKS * 7> grid{};

  // Summary over the whole window (not only the displayed weeks).
  uint16_t daysRead = 0;
  uint32_t totalMinutes = 0;
  uint16_t longestStreak = 0;
  uint16_t spanDays = 0;
  bool anyEstimated = false;

  bool valid() const { return weekCount > 0; }
  // Value for a day; zero outside the displayed weeks or the window.
  Day dayAt(uint32_t dayIndex) const;

  // todayDay is today's day index, or 0 when no clock date is available.
  // weekStart takes CrossPointSettings::WEEK_START_DAY values and only decides
  // which weekday each displayed week begins on.
  // Returns false when the book has no usable start date.
  static bool build(const BookReadingStats& stats, uint32_t todayDay, uint32_t historyAnchorDay,
                    const std::array<uint8_t, READING_HISTORY_BYTES>& historyBits, const BookDailyMinutes& real,
                    uint8_t weekStart, BookRhythm& out);
};
