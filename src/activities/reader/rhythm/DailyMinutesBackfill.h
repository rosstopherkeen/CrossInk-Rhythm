#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "DailyMinutesLog.h"
#include "activities/reader/ReadingStatsUtils.h"

// One-time estimate of daily reading minutes for days read before this
// firmware started recording real minutes. Only real read days (bits in
// GlobalReadingStats::readingHistoryBits) ever receive minutes.
//
// Rules:
//   - Each book's totalReadingSeconds is spread evenly over k of its real read
//     days between its start date and its finish date (ongoing: today), where
//     k = min(sessionCount, readDaysInRange). The k days are spaced evenly
//     across the read days in that range.
//   - Real read days not covered by any book get 1 minute.
//   - Only days on or before cutoffDay are estimated; later days are fully
//     covered by real recordings.
//   - Estimates go in DailyMinutesLog::estimatedMinutes, so real minutes
//     recorded for the same day always win at display time.
struct DailyMinutesBackfillBook {
  uint32_t startDay = 0;  // readingStatsDayIndex() of the start date
  uint32_t endDay = 0;    // finish date, or today for the ongoing book
  uint32_t totalReadingSeconds = 0;
  uint16_t sessionCount = 0;
};

namespace DailyMinutesBackfill {

// Minutes per chosen day: round(totalSeconds / 60 / k), at least 1.
uint32_t minutesPerDay(uint32_t totalSeconds, uint32_t dayCount);

// Pure computation: fills log.estimatedMinutes and sets the backfill flag.
// The caller must already have advanced log to today.
void apply(uint32_t historyAnchorDay, const std::array<uint8_t, READING_HISTORY_BYTES>& historyBits, uint32_t cutoffDay,
           const DailyMinutesBackfillBook* books, size_t bookCount, DailyMinutesLog& log);

// Device path: loads the log, gathers finished books plus the ongoing book from
// their stats_v5.bin files (read-only), applies the backfill once and saves.
// Returns the up-to-date log for display. No-op when already backfilled or
// when no RTC date is available.
DailyMinutesLog runOnceIfNeeded(uint32_t historyAnchorDay,
                                const std::array<uint8_t, READING_HISTORY_BYTES>& historyBits);

}  // namespace DailyMinutesBackfill
