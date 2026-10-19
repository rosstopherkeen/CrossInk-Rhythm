// Pure per-book rhythm derivation; no storage access so host tests can run it.
#include "BookRhythm.h"

#include <algorithm>
#include <limits>
#include <vector>

#include "DailyMinutesBackfill.h"

namespace {

// 2000-01-01 (day 0) was a Saturday; Monday = 0 as in readingStatsDayOfWeekIndex().
uint32_t mondayOf(const uint32_t dayIndex) {
  const uint32_t dow = (5u + dayIndex) % 7u;
  return dayIndex >= dow ? dayIndex - dow : 0;
}

}  // namespace

BookRhythm::Day BookRhythm::dayAt(const uint32_t dayIndex) const {
  if (!valid() || dayIndex < firstShownDay || dayIndex < startDay || dayIndex > endDay) return {};
  const uint32_t offset = dayIndex - firstShownDay;
  return offset < grid.size() ? grid[offset] : Day{};
}

bool BookRhythm::build(const BookReadingStats& stats, const uint32_t todayDay, const uint32_t historyAnchorDay,
                       const std::array<uint8_t, READING_HISTORY_BYTES>& historyBits, const BookDailyMinutes& real,
                       BookRhythm& out) {
  out = BookRhythm{};
  if (!stats.startDate.isValid()) return false;
  const uint32_t start = readingStatsDayIndex(stats.startDate);
  uint32_t end = 0;
  if (stats.isCompleted && stats.finishedDate.isValid()) {
    end = readingStatsDayIndex(stats.finishedDate);
  } else if (todayDay != 0) {
    end = todayDay;
  } else {
    end = std::max(start, historyAnchorDay);
  }
  if (end < start) end = start;
  out.startDay = start;
  out.endDay = end;
  out.spanDays = static_cast<uint16_t>(std::min<uint32_t>(UINT16_MAX, end - start + 1));

  // Real per-book minutes inside the window (entries from before a stats reset
  // or after the finish date are ignored).
  std::vector<BookDailyMinutes::Day> merged;
  merged.reserve(real.days.size() + 16);
  uint32_t realMinutes = 0;
  uint32_t firstReal = std::numeric_limits<uint32_t>::max();
  for (const auto& d : real.days) {
    if (d.day < start || d.day > end || d.minutes == 0) continue;
    merged.push_back(d);
    realMinutes += d.minutes;
    firstReal = std::min(firstReal, d.day);
  }
  const size_t realCount = merged.size();

  // Estimates for the days before the first real recording.
  const uint32_t estimateLast = firstReal == std::numeric_limits<uint32_t>::max() ? end : firstReal - 1;
  const uint64_t realSeconds = static_cast<uint64_t>(realMinutes) * 60u;
  const uint32_t remainingSeconds =
      stats.totalReadingSeconds > realSeconds ? static_cast<uint32_t>(stats.totalReadingSeconds - realSeconds) : 0;
  if (estimateLast >= start && remainingSeconds > 0) {
    struct Context {
      std::vector<BookDailyMinutes::Day>* days;
    } context{&merged};
    DailyMinutesBackfill::spreadEvenly(
        historyAnchorDay, historyBits, start, estimateLast, stats.sessionCount, remainingSeconds,
        [](const uint32_t day, const uint32_t minutes, void* ctx) {
          static_cast<Context*>(ctx)->days->push_back(
              {day, static_cast<uint16_t>(std::min<uint32_t>(minutes, BookDailyMinutes::MAX_MINUTES_PER_DAY))});
        },
        &context);
  }
  const size_t estimateBegin = realCount;
  std::vector<bool> isEstimate(merged.size(), false);
  for (size_t i = estimateBegin; i < merged.size(); ++i) isEstimate[i] = true;

  // Sort by day (real and estimated ranges never overlap).
  std::vector<size_t> order(merged.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(),
            [&merged](const size_t a, const size_t b) { return merged[a].day < merged[b].day; });

  // Displayed weeks: whole window, or its last MAX_WEEKS weeks.
  const uint32_t firstMonday = mondayOf(start);
  const uint32_t lastMonday = mondayOf(end);
  const uint32_t weeks = (lastMonday - firstMonday) / 7u + 1u;
  out.truncated = weeks > static_cast<uint32_t>(MAX_WEEKS);
  out.weekCount = static_cast<int>(std::min<uint32_t>(weeks, MAX_WEEKS));
  out.firstShownDay = out.truncated ? lastMonday - static_cast<uint32_t>(MAX_WEEKS - 1) * 7u : firstMonday;

  uint32_t previousDay = 0;
  uint16_t streak = 0;
  for (const size_t index : order) {
    const auto& d = merged[index];
    ++out.daysRead;
    out.totalMinutes += d.minutes;
    if (isEstimate[index]) out.anyEstimated = true;
    streak = (streak > 0 && d.day == previousDay + 1) ? static_cast<uint16_t>(streak + 1) : 1;
    out.longestStreak = std::max(out.longestStreak, streak);
    previousDay = d.day;
    if (d.day >= out.firstShownDay) {
      const uint32_t offset = d.day - out.firstShownDay;
      if (offset < out.grid.size()) out.grid[offset] = Day{d.minutes, isEstimate[index]};
    }
  }
  return true;
}
