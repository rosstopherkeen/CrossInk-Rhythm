// Pure backfill math for the Reading Rhythm minutes estimate. No storage
// access here so it runs unchanged in host tests.
#include "DailyMinutesBackfill.h"

#include <algorithm>

namespace {

bool isReadDay(const uint32_t historyAnchorDay, const std::array<uint8_t, READING_HISTORY_BYTES>& bits,
               const uint32_t dayIndex) {
  if (dayIndex > historyAnchorDay) return false;
  const uint32_t offset = historyAnchorDay - dayIndex;
  if (offset >= READING_HISTORY_DAYS) return false;
  return (bits[offset / 8] & static_cast<uint8_t>(1u << (offset % 8))) != 0;
}

bool historyIsEmpty(const uint32_t historyAnchorDay, const std::array<uint8_t, READING_HISTORY_BYTES>& bits) {
  return historyAnchorDay == 0 && std::all_of(bits.begin(), bits.end(), [](const uint8_t b) { return b == 0; });
}

}  // namespace

namespace DailyMinutesBackfill {

uint32_t minutesPerDay(const uint32_t totalSeconds, const uint32_t dayCount) {
  if (dayCount == 0) return 0;
  const uint64_t divisor = static_cast<uint64_t>(dayCount) * 60u;
  const uint64_t rounded = (static_cast<uint64_t>(totalSeconds) + divisor / 2u) / divisor;
  return static_cast<uint32_t>(std::max<uint64_t>(1u, rounded));
}

void apply(const uint32_t historyAnchorDay, const std::array<uint8_t, READING_HISTORY_BYTES>& historyBits,
           const uint32_t cutoffDay, const DailyMinutesBackfillBook* books, const size_t bookCount,
           DailyMinutesLog& log) {
  log.flags |= DailyMinutesLog::FLAG_BACKFILL_DONE;
  if (log.isEmpty() || historyIsEmpty(historyAnchorDay, historyBits)) return;

  // Days the history bits can describe at all.
  const uint32_t historyFirstDay =
      historyAnchorDay >= READING_HISTORY_DAYS - 1 ? historyAnchorDay - (READING_HISTORY_DAYS - 1) : 0;
  const uint32_t lastEstimatedDay = std::min(cutoffDay, historyAnchorDay);

  for (size_t b = 0; b < bookCount; ++b) {
    const DailyMinutesBackfillBook& book = books[b];
    if (book.startDay == 0 || book.endDay < book.startDay || book.totalReadingSeconds == 0) continue;
    const uint32_t first = std::max(book.startDay, historyFirstDay);
    const uint32_t last = std::min(book.endDay, lastEstimatedDay);
    if (last < first) continue;

    uint32_t readDays = 0;
    for (uint32_t day = first; day <= last; ++day) {
      if (isReadDay(historyAnchorDay, historyBits, day)) ++readDays;
    }
    if (readDays == 0) continue;

    const uint32_t chosen = std::min<uint32_t>(std::max<uint16_t>(book.sessionCount, 1), readDays);
    const uint32_t perDay = minutesPerDay(book.totalReadingSeconds, chosen);

    // Evenly spaced picks: the i-th chosen day is read day number i*readDays/chosen.
    uint32_t ordinal = 0;
    uint32_t pickIndex = 0;
    uint32_t nextOrdinal = 0;
    for (uint32_t day = first; day <= last && pickIndex < chosen; ++day) {
      if (!isReadDay(historyAnchorDay, historyBits, day)) continue;
      if (ordinal == nextOrdinal) {
        log.addEstimatedMinutes(day, perDay);
        ++pickIndex;
        nextOrdinal = static_cast<uint32_t>((static_cast<uint64_t>(pickIndex) * readDays) / chosen);
      }
      ++ordinal;
    }
  }

  // Remaining real read days inside the minutes window get a 1-minute marker.
  const uint32_t windowFirstDay =
      log.anchorDay >= DailyMinutesLog::WINDOW_DAYS - 1 ? log.anchorDay - (DailyMinutesLog::WINDOW_DAYS - 1) : 0;
  const uint32_t windowLastDay = std::min(log.anchorDay, lastEstimatedDay);
  for (uint32_t day = std::max(windowFirstDay, historyFirstDay); day <= windowLastDay; ++day) {
    if (isReadDay(historyAnchorDay, historyBits, day) && log.estimatedMinutesOnDay(day) == 0) {
      log.addEstimatedMinutes(day, 1);
    }
  }
}

}  // namespace DailyMinutesBackfill
