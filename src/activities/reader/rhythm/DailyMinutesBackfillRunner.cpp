// Device-side driver for the one-time Reading Rhythm minutes backfill. The
// math lives in DailyMinutesBackfill.cpp so host tests can run it without SD.
#include <Logging.h>

#include <vector>

#include "BookStatsScan.h"
#include "CrossPointState.h"
#include "DailyMinutesBackfill.h"

namespace {
struct CollectContext {
  std::vector<DailyMinutesBackfillBook> books;
  std::string ongoingCachePath;
  uint32_t today = 0;
};

void collectBackfillBook(const BookStatsScan::Book& book, void* ctx) {
  auto& context = *static_cast<CollectContext*>(ctx);
  const BookReadingStats& stats = book.stats;
  if (!stats.startDate.isValid() || stats.totalReadingSeconds == 0) return;

  DailyMinutesBackfillBook entry;
  entry.startDay = readingStatsDayIndex(stats.startDate);
  entry.totalReadingSeconds = stats.totalReadingSeconds;
  entry.sessionCount = stats.sessionCount;
  if (stats.isCompleted) {
    if (!stats.finishedDate.isValid()) return;
    entry.endDay = readingStatsDayIndex(stats.finishedDate);
  } else if (!context.ongoingCachePath.empty() && book.cachePath == context.ongoingCachePath) {
    entry.endDay = context.today;
  } else {
    return;
  }
  context.books.push_back(entry);
}
}  // namespace

namespace DailyMinutesBackfill {

DailyMinutesLog runOnceIfNeeded(const uint32_t historyAnchorDay,
                                const std::array<uint8_t, READING_HISTORY_BYTES>& historyBits) {
  DailyMinutesLog log = DailyMinutesLog::load();
  if (log.backfillDone()) return log;

  ReadingStatsDateTime now;
  if (!getCurrentLocalReadingStatsDateTime(now)) {
    LOG_DBG("DMIN", "No clock date yet; deferring minutes backfill");
    return log;
  }

  CollectContext context;
  context.today = readingStatsDayIndex(now.date);
  context.ongoingCachePath = BookStatsScan::cachePathForBookPath(APP_STATE.openEpubPath);
  context.books.reserve(32);
  BookStatsScan::forEach(collectBackfillBook, &context);

  // Days on or after the first real recording are covered by real minutes.
  const uint32_t cutoffDay = log.firstRealDay != 0 ? log.firstRealDay : context.today;
  log.advanceTo(context.today);
  apply(historyAnchorDay, historyBits, cutoffDay, context.books.data(), context.books.size(), log);
  if (!log.save()) {
    LOG_ERR("DMIN", "Could not save minutes backfill");
  } else {
    LOG_INF("DMIN", "Backfilled reading minutes from %u books", static_cast<unsigned>(context.books.size()));
  }
  return log;
}

}  // namespace DailyMinutesBackfill
