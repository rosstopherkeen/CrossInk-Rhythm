// Ported from XPoint (https://github.com/Belphemur/XPoint, MIT):
// src/activities/reader/BookStatsView.cpp renderReadingRhythmPage() and
// renderFinishedBooksPage(). Chart math (dot intensities, weekly scale steps,
// month buckets) is unchanged; layout follows the Reading Stats card frame.
#include "RhythmStatsView.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <numeric>
#include <string>

#include "MappedInputManager.h"
#include "activities/reader/BookStatsView.h"
#include "activities/reader/ReadingStatsUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr int kReadingRhythmWeekCount = 13;
constexpr size_t kReadingRhythmMonthCount = 12;
constexpr int kRecentReadingDayCount = 7;
constexpr uint16_t kMediumReadingMinutes = 15;
constexpr uint16_t kHighReadingMinutes = 30;
constexpr int kCardPadding = 10;

constexpr std::array<StrId, READING_DAY_OF_WEEK_COUNT> DAY_LABELS = {
    StrId::STR_STATS_MON, StrId::STR_STATS_TUE, StrId::STR_STATS_WED, StrId::STR_STATS_THU,
    StrId::STR_STATS_FRI, StrId::STR_STATS_SAT, StrId::STR_STATS_SUN};

// Minutes shown for one day. Only days present in the global read-day history
// count, so a stats reset (which clears the bits) also hides stale minutes.
// A read day without any minute data shows 1 estimated minute.
struct DayMinutes {
  uint16_t minutes = 0;
  bool estimated = false;
};

bool isReadDay(const GlobalReadingStats& stats, const uint32_t dayIndex) {
  if (dayIndex > stats.readingHistoryAnchorDay) return false;
  const uint32_t offset = stats.readingHistoryAnchorDay - dayIndex;
  if (offset >= READING_HISTORY_DAYS) return false;
  return (stats.readingHistoryBits[offset / 8] & static_cast<uint8_t>(1u << (offset % 8))) != 0;
}

DayMinutes minutesOnDay(const GlobalReadingStats& stats, const DailyMinutesLog& log, const uint32_t dayIndex) {
  DayMinutes result;
  if (!isReadDay(stats, dayIndex)) return result;
  result.minutes = log.minutesOnDay(dayIndex, &result.estimated);
  if (result.minutes == 0) {
    result.minutes = 1;
    result.estimated = true;
  }
  return result;
}

const char* dayCountText(const uint16_t days) { return days == 1 ? tr(STR_STATS_DAY) : tr(STR_STATS_DAYS); }

void formatRhythmMinutes(const uint32_t minutes, char* buf, const size_t len) {
  if (minutes < 60) {
    snprintf(buf, len, tr(STR_RHYTHM_DURATION_MIN), static_cast<unsigned long>(minutes));
    return;
  }
  const uint32_t hours = minutes / 60u;
  const uint32_t remainder = minutes % 60u;
  if (remainder == 0) {
    snprintf(buf, len, tr(STR_RHYTHM_DURATION_H), static_cast<unsigned long>(hours));
  } else {
    snprintf(buf, len, tr(STR_RHYTHM_DURATION_HM), static_cast<unsigned long>(hours),
             static_cast<unsigned long>(remainder));
  }
}

void readingIntensityStyle(const uint16_t minutes, const int activeDotSize, const int inactiveDotSize, int& dotSize,
                           Color& color) {
  if (minutes == 0) {
    dotSize = inactiveDotSize;
    color = Color::LightGray;
  } else if (minutes < kMediumReadingMinutes) {
    dotSize = std::max(inactiveDotSize + 1, activeDotSize - 4);
    color = Color::LightGray;
  } else if (minutes < kHighReadingMinutes) {
    dotSize = std::max(inactiveDotSize + 2, activeDotSize - 2);
    color = Color::DarkGray;
  } else {
    dotSize = activeDotSize;
    color = Color::Black;
  }
}

uint16_t weeklyChartScaleMinutes(const uint32_t maxMinutes) {
  if (maxMinutes <= 60) return 60;
  if (maxMinutes <= 180) return 180;
  if (maxMinutes <= 360) return 360;
  if (maxMinutes <= 600) return 600;
  return static_cast<uint16_t>(std::min<uint32_t>(UINT16_MAX, ((maxMinutes + 119u) / 120u) * 120u));
}

void drawCenteredLabel(const GfxRenderer& renderer, const int fontId, const int x, const int w, const int y,
                       const char* text, const bool bold = false) {
  const auto style = bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  renderer.drawText(fontId, x + (w - renderer.getTextWidth(fontId, text, style)) / 2, y, text, true, style);
}

// Same hint set as the built-in stats pages: Back exits, Left/Right page.
void drawButtonHints(GfxRenderer& renderer, const MappedInputManager* mappedInput, const bool previousIsPage,
                     const char* next) {
  if (!mappedInput) return;
  const MappedInputManager::Label previous =
      previousIsPage ? MappedInputManager::Label(tr(STR_RHYTHM_PREV_SHORT)) : mappedInput->withBackArrow(tr(STR_BACK));
  const auto labels = mappedInput->mapLabels(mappedInput->withBackArrow(tr(STR_EXIT)), "", previous, next);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
}

// "~5h 14m" style value for the 7-day strip, shortened until it fits.
void fitRecentDayValue(const GfxRenderer& renderer, const DayMinutes& day, const int maxWidth, char* out,
                       const size_t len) {
  char compact[16];
  formatCompactReadingDuration(static_cast<uint32_t>(day.minutes) * 60u, compact, sizeof(compact));
  snprintf(out, len, "%s%s", day.estimated ? "~" : "", compact);
  if (renderer.getTextWidth(SMALL_FONT_ID, out) <= maxWidth) return;
  std::string squeezed = compact;
  squeezed.erase(std::remove(squeezed.begin(), squeezed.end(), ' '), squeezed.end());
  snprintf(out, len, "%s%s", day.estimated ? "~" : "", squeezed.c_str());
  if (renderer.getTextWidth(SMALL_FONT_ID, out) <= maxWidth) return;
  snprintf(out, len, "%s", squeezed.c_str());
}

// ---- Reading Rhythm card heights ------------------------------------------

struct RhythmMetrics {
  int smallH;
  int monthLabelH;
  int recentH;
  int minRowStride;
  int maxRowStride;
  int rhythmMinH;
  int weeklyMinH;
  int monthlyMinH;
};

RhythmMetrics rhythmMetrics(const GfxRenderer& renderer, const int titleH, const bool compactWidth) {
  RhythmMetrics m{};
  m.smallH = renderer.getLineHeight(SMALL_FONT_ID);
  m.monthLabelH = m.smallH + 6;
  m.recentH = m.smallH * 2 + 12;
  m.minRowStride = compactWidth ? 20 : 24;
  m.maxRowStride = compactWidth ? 34 : 40;
  m.rhythmMinH = titleH + m.monthLabelH + 7 * m.minRowStride + m.recentH + kCardPadding;
  m.weeklyMinH = titleH + 70 + m.smallH + 8;
  m.monthlyMinH = titleH + m.smallH * 3 + 46;
  return m;
}

// ---- Books Finished list rows ----------------------------------------------

struct FinishedListMetrics {
  StatsPageFrame frame;
  int summaryH;
  int listTop;     // first row y inside the list card
  int listBottom;  // last usable y inside the list card
  int headerRowH;
  int headerGap;  // extra space above a month header that follows a book
  int bookRowH;
};

FinishedListMetrics finishedListMetrics(const GfxRenderer& renderer, const MappedInputManager* mappedInput) {
  const int smallH = renderer.getLineHeight(SMALL_FONT_ID);
  const int bodyH = renderer.getLineHeight(UI_10_FONT_ID);
  const int valueH = renderer.getLineHeight(UI_12_FONT_ID);
  FinishedListMetrics m{};
  m.headerRowH = smallH + 10;
  m.headerGap = 8;
  m.bookRowH = bodyH + smallH + 12;
  const int minSummaryH = 30 + valueH + smallH + 20;
  m.frame = statsPageFrame(renderer, mappedInput, true, minSummaryH + 30 + m.headerRowH + m.bookRowH * 3, 2);
  m.summaryH = m.frame.titleH + valueH + smallH + 24;
  const int listCardY = m.frame.top + m.summaryH + m.frame.cardGap;
  m.listTop = listCardY + m.frame.titleH + 6;
  m.listBottom = m.frame.bottom - 6;
  return m;
}

bool sameFinishedMonth(const ReadingStatsDate& a, const ReadingStatsDate& b) {
  return a.isValid() && b.isValid() && a.year == b.year && a.month == b.month;
}

// A month header precedes the first row of each page and every month change.
bool startsMonthSection(const std::vector<FinishedBookEntry>& books, const size_t index, const size_t pageStart) {
  return index == pageStart || !sameFinishedMonth(books[index].finishedDate, books[index - 1].finishedDate);
}

void formatMonthHeader(const std::vector<FinishedBookEntry>& books, const ReadingStatsDate& date, char* buf,
                       const size_t len) {
  if (!date.isValid()) {
    snprintf(buf, len, "-");
    return;
  }
  unsigned count = 0;
  uint32_t seconds = 0;
  for (const auto& entry : books) {
    if (!sameFinishedMonth(entry.finishedDate, date)) continue;
    ++count;
    seconds += entry.totalReadingSeconds;
  }
  char month[8];
  char duration[24];
  formatReadingStatsMonthToken(date, month, sizeof(month));
  formatRhythmMinutes(seconds / 60u, duration, sizeof(duration));
  snprintf(buf, len, tr(STR_FINISHED_MONTH_FORMAT), month, static_cast<unsigned>(date.year), count,
           count == 1 ? tr(STR_FINISHED_BOOK_ONE) : tr(STR_FINISHED_BOOK_MANY), duration);
}

}  // namespace

namespace RhythmStatsView {

void renderReadingRhythmPage(GfxRenderer& renderer, const MappedInputManager* mappedInput,
                             const GlobalReadingStats& stats, const DailyMinutesLog& minutesLog,
                             const bool showMoreButton) {
  const bool compactWidth = renderer.getScreenWidth() < 500;
  // Title band height does not depend on the layout choice enough to matter for
  // the minimum; use the compact band to size the minimum content.
  const RhythmMetrics base = rhythmMetrics(renderer, 30, compactWidth);
  const StatsPageFrame frame =
      statsPageFrame(renderer, mappedInput, true, base.rhythmMinH + base.weeklyMinH + base.monthlyMinH, 3);
  drawStatsPageHeader(renderer, mappedInput, tr(STR_READING_RHYTHM));
  const RhythmMetrics m = rhythmMetrics(renderer, frame.titleH, compactWidth);

  // Size the three cards so they exactly fill the frame: grow the dot grid rows
  // first (up to a comfortable stride), then give the rest to the two charts.
  const int availableH = frame.bottom - frame.top;
  const bool showMonthlyChart = availableH >= m.rhythmMinH + m.weeklyMinH + m.monthlyMinH + frame.cardGap * 2;
  const int fixedH =
      m.rhythmMinH + m.weeklyMinH + (showMonthlyChart ? m.monthlyMinH + frame.cardGap : 0) + frame.cardGap;
  int extraH = std::max(0, availableH - fixedH);
  const int rowExtra = std::min(m.maxRowStride - m.minRowStride, (extraH / 2) / 7);
  const int rowStride = m.minRowStride + rowExtra;
  extraH -= rowExtra * 7;
  const int weeklyExtra = showMonthlyChart ? extraH / 2 : extraH;
  const int monthlyExtra = showMonthlyChart ? extraH - weeklyExtra : 0;
  const int rhythmCardH = m.rhythmMinH + rowExtra * 7;
  const int weeklyChartH = m.weeklyMinH + weeklyExtra;
  const int monthlyChartH = m.monthlyMinH + monthlyExtra;

  const int cardX = frame.cardX;
  const int cardW = frame.cardW;
  const int titleH = frame.titleH;
  const int weekdayLabelW = compactWidth ? 38 : 46;
  const int activeDotSize = std::clamp(rowStride - 10, 12, 18);
  const int inactiveDotSize = compactWidth ? 5 : 6;

  ReadingStatsDateTime now;
  const bool hasToday = getCurrentLocalReadingStatsDateTime(now);
  const uint32_t displayDay = hasToday ? readingStatsDayIndex(now.date) : stats.readingHistoryAnchorDay;
  ReadingStatsDate displayDate;
  const bool hasDisplayDate = readingStatsDateFromDayIndex(displayDay, displayDate);
  const uint8_t displayDow = hasDisplayDate ? readingStatsDayOfWeekIndex(displayDate) : 0;
  const uint32_t currentWeekStart = displayDay >= displayDow ? displayDay - displayDow : 0;
  const uint32_t historySpanBeforeCurrentWeek = static_cast<uint32_t>(kReadingRhythmWeekCount - 1) * 7u;
  const uint32_t firstDay =
      currentWeekStart >= historySpanBeforeCurrentWeek ? currentWeekStart - historySpanBeforeCurrentWeek : 0;

  // ---- Recent Activity card
  const int rhythmY = frame.top;
  drawStatsCardFrame(renderer, cardX, rhythmY, cardW, rhythmCardH, tr(STR_RHYTHM_RECENT_ACTIVITY), titleH);

  const int gridX = cardX + kCardPadding + weekdayLabelW;
  const int gridW = cardW - kCardPadding * 2 - weekdayLabelW;
  const int columnStride = std::max(1, gridW / kReadingRhythmWeekCount);
  const int gridTop = rhythmY + titleH + m.monthLabelH;
  bool anyEstimated = false;

  uint8_t previousMonth = 0;
  for (int week = 0; week < kReadingRhythmWeekCount; ++week) {
    ReadingStatsDate weekDate;
    if (readingStatsDateFromDayIndex(firstDay + static_cast<uint32_t>(week) * 7u, weekDate) &&
        weekDate.month != previousMonth) {
      char monthToken[8];
      formatReadingStatsMonthToken(weekDate, monthToken, sizeof(monthToken));
      renderer.drawText(SMALL_FONT_ID, gridX + week * columnStride, rhythmY + titleH + 3, monthToken);
      previousMonth = weekDate.month;
    }
  }

  for (int weekday = 0; weekday < 7; ++weekday) {
    const int rowCenterY = gridTop + weekday * rowStride + rowStride / 2;
    renderer.drawText(SMALL_FONT_ID, cardX + kCardPadding, rowCenterY - m.smallH / 2,
                      I18n::getInstance().get(DAY_LABELS[weekday]));

    for (int week = 0; week < kReadingRhythmWeekCount; ++week) {
      const uint32_t dayIndex = firstDay + static_cast<uint32_t>(week) * 7u + static_cast<uint32_t>(weekday);
      if (dayIndex > displayDay) continue;

      const int centerX = gridX + week * columnStride + columnStride / 2;
      const DayMinutes day = minutesOnDay(stats, minutesLog, dayIndex);
      int dotSize = inactiveDotSize;
      Color dotColor = Color::LightGray;
      readingIntensityStyle(day.minutes, activeDotSize, inactiveDotSize, dotSize, dotColor);
      const int dotX = centerX - dotSize / 2;
      const int dotY = rowCenterY - dotSize / 2;
      if (day.estimated) {
        anyEstimated = true;
        renderer.drawRoundedRect(dotX, dotY, dotSize, dotSize, 1, dotSize / 2, true);
      } else {
        renderer.fillRoundedRect(dotX, dotY, dotSize, dotSize, dotSize / 2, dotColor);
      }

      if (hasToday && dayIndex == displayDay) {
        const int ringSize = activeDotSize + 6;
        renderer.drawRoundedRect(centerX - ringSize / 2, rowCenterY - ringSize / 2, ringSize, ringSize, 2, ringSize / 2,
                                 true);
      }
    }
  }

  if (anyEstimated) {
    // Legend in the title band, right of the centered title when it fits.
    const char* note = tr(STR_RHYTHM_ESTIMATED_NOTE);
    const int noteW = renderer.getTextWidth(SMALL_FONT_ID, note);
    const int titleW = renderer.getTextWidth(UI_10_FONT_ID, tr(STR_RHYTHM_RECENT_ACTIVITY), EpdFontFamily::BOLD);
    const int noteX = cardX + cardW - kCardPadding - noteW;
    if (noteX > cardX + (cardW + titleW) / 2 + 8) {
      renderer.drawText(SMALL_FONT_ID, noteX, rhythmY + (titleH - m.smallH) / 2, note);
    }
  }

  const int recentDaysY = gridTop + rowStride * 7;
  renderer.drawLine(cardX, recentDaysY, cardX + cardW, recentDaysY);
  const int recentDayW = std::max(1, (cardW - kCardPadding * 2) / kRecentReadingDayCount);
  const int recentDayLabelY = recentDaysY + 5;
  const int recentDayValueY = recentDayLabelY + m.smallH + 2;
  const uint32_t firstRecentDay =
      displayDay >= kRecentReadingDayCount - 1 ? displayDay - static_cast<uint32_t>(kRecentReadingDayCount - 1) : 0;
  for (int dayOffset = 0; dayOffset < kRecentReadingDayCount; ++dayOffset) {
    const uint32_t dayIndex = firstRecentDay + static_cast<uint32_t>(dayOffset);
    const int columnX = cardX + kCardPadding + dayOffset * recentDayW;
    ReadingStatsDate date;
    if (!readingStatsDateFromDayIndex(dayIndex, date)) continue;

    if (hasToday && dayIndex == displayDay) {
      renderer.drawRoundedRect(columnX + 1, recentDaysY + 3, recentDayW - 2, m.recentH - 6, 1, 4, true);
    }
    drawCenteredLabel(renderer, SMALL_FONT_ID, columnX, recentDayW, recentDayLabelY,
                      I18n::getInstance().get(DAY_LABELS[readingStatsDayOfWeekIndex(date)]));

    const DayMinutes day = minutesOnDay(stats, minutesLog, dayIndex);
    char value[20];
    if (day.minutes == 0) {
      snprintf(value, sizeof(value), "-");
    } else {
      fitRecentDayValue(renderer, day, recentDayW - 4, value, sizeof(value));
    }
    drawCenteredLabel(renderer, SMALL_FONT_ID, columnX, recentDayW, recentDayValueY, value);
  }

  // ---- Weekly Reading Time card
  std::array<uint32_t, kReadingRhythmWeekCount> weeklyMinutes{};
  std::array<uint32_t, kReadingRhythmWeekCount> weeklyEstimatedMinutes{};
  uint32_t previousWeeksMinutes = 0;
  for (int week = 0; week < kReadingRhythmWeekCount; ++week) {
    for (int weekday = 0; weekday < 7; ++weekday) {
      const uint32_t dayIndex = firstDay + static_cast<uint32_t>(week) * 7u + static_cast<uint32_t>(weekday);
      if (dayIndex > displayDay) continue;
      const DayMinutes day = minutesOnDay(stats, minutesLog, dayIndex);
      weeklyMinutes[static_cast<size_t>(week)] += day.minutes;
      if (day.estimated) weeklyEstimatedMinutes[static_cast<size_t>(week)] += day.minutes;
    }
    if (week < kReadingRhythmWeekCount - 1) previousWeeksMinutes += weeklyMinutes[static_cast<size_t>(week)];
  }
  const uint32_t maxWeeklyMinutes = *std::max_element(weeklyMinutes.begin(), weeklyMinutes.end());
  const uint16_t chartScaleMinutes = weeklyChartScaleMinutes(maxWeeklyMinutes);
  const uint32_t averageWeeklyMinutes = previousWeeksMinutes / static_cast<uint32_t>(kReadingRhythmWeekCount - 1);

  const int chartY = rhythmY + rhythmCardH + frame.cardGap;
  drawStatsCardFrame(renderer, cardX, chartY, cardW, weeklyChartH, tr(STR_RHYTHM_WEEKLY_TIME), titleH);
  const int footerH = m.smallH + 6;
  const int chartTop = chartY + titleH + 8;
  const int chartBottom = chartY + weeklyChartH - footerH - 4;
  const int chartHeight = std::max(1, chartBottom - chartTop);
  const int chartRight = gridX + columnStride * kReadingRhythmWeekCount;

  char scaleLabel[16];
  formatRhythmMinutes(chartScaleMinutes, scaleLabel, sizeof(scaleLabel));
  renderer.drawText(SMALL_FONT_ID, cardX + kCardPadding, chartTop, scaleLabel);
  renderer.drawLine(gridX, chartBottom, chartRight, chartBottom);

  if (averageWeeklyMinutes > 0) {
    const int averageY =
        chartBottom - static_cast<int>((static_cast<uint64_t>(chartHeight) * averageWeeklyMinutes) / chartScaleMinutes);
    for (int dashX = gridX; dashX < chartRight; dashX += 8) {
      renderer.drawLine(dashX, averageY, std::min(dashX + 3, chartRight), averageY);
    }
  }

  const int barWidth = std::min(columnStride - 4, compactWidth ? 14 : 18);
  for (int week = 0; week < kReadingRhythmWeekCount; ++week) {
    const uint32_t minutes = weeklyMinutes[static_cast<size_t>(week)];
    if (minutes == 0) continue;
    const int barHeight = std::min(
        chartHeight, std::max(2, static_cast<int>((static_cast<uint64_t>(chartHeight) * minutes) / chartScaleMinutes)));
    const uint32_t realMinutes = minutes - weeklyEstimatedMinutes[static_cast<size_t>(week)];
    const int realHeight =
        realMinutes == 0 ? 0 : std::max(2, static_cast<int>((static_cast<int64_t>(barHeight) * realMinutes) / minutes));
    const int barX = gridX + week * columnStride + columnStride / 2 - barWidth / 2;
    if (realHeight > 0) {
      if (week == kReadingRhythmWeekCount - 1) {
        renderer.fillRect(barX, chartBottom - realHeight, barWidth, realHeight);
      } else {
        renderer.fillRectDither(barX, chartBottom - realHeight, barWidth, realHeight, Color::DarkGray);
      }
    }
    // Estimated share sits on top of the real share: light, outlined segment.
    const int estimatedHeight = barHeight - realHeight;
    if (estimatedHeight > 0) {
      const int estimatedTop = chartBottom - barHeight;
      renderer.fillRectDither(barX, estimatedTop, barWidth, estimatedHeight, Color::LightGray);
      renderer.drawRect(barX, estimatedTop, barWidth, estimatedHeight);
    }
  }

  char averageValue[16];
  char currentValue[16];
  formatRhythmMinutes(averageWeeklyMinutes, averageValue, sizeof(averageValue));
  formatRhythmMinutes(weeklyMinutes.back(), currentValue, sizeof(currentValue));
  const int footerY = chartY + weeklyChartH - footerH + 1;
  const char* averageText = tr(STR_RHYTHM_WEEKLY_AVERAGE);
  renderer.drawText(SMALL_FONT_ID, cardX + kCardPadding, footerY, averageText);
  renderer.drawText(SMALL_FONT_ID, cardX + kCardPadding + renderer.getTextWidth(SMALL_FONT_ID, averageText) + 4,
                    footerY, averageValue);
  const char* currentText = tr(STR_RHYTHM_THIS_WEEK);
  const int currentTextW = renderer.getTextWidth(SMALL_FONT_ID, currentText);
  const int currentX =
      cardX + cardW - kCardPadding - currentTextW - 4 - renderer.getTextWidth(SMALL_FONT_ID, currentValue);
  renderer.drawText(SMALL_FONT_ID, currentX, footerY, currentText);
  renderer.drawText(SMALL_FONT_ID, currentX + currentTextW + 4, footerY, currentValue);

  // ---- Reading Days by Month card
  if (showMonthlyChart) {
    std::array<ReadingStatsDate, kReadingRhythmMonthCount> monthDates{};
    std::array<uint8_t, kReadingRhythmMonthCount> monthlyReadingDays{};
    ReadingStatsDate monthCursor = displayDate;
    monthCursor.day = 1;
    for (size_t offset = 0; offset < kReadingRhythmMonthCount; ++offset) {
      monthDates[kReadingRhythmMonthCount - 1 - offset] = monthCursor;
      addDaysToReadingStatsDate(monthCursor, -1);
      monthCursor.day = 1;
    }
    // Read days per month come straight from the 730-day history bits.
    for (size_t monthIndex = 0; monthIndex < kReadingRhythmMonthCount; ++monthIndex) {
      const uint32_t monthStartDay = readingStatsDayIndex(monthDates[monthIndex]);
      const uint8_t monthDayCount = daysInMonth(monthDates[monthIndex].year, monthDates[monthIndex].month);
      for (uint8_t dayOffset = 0; dayOffset < monthDayCount; ++dayOffset) {
        if (isReadDay(stats, monthStartDay + dayOffset)) ++monthlyReadingDays[monthIndex];
      }
    }
    const uint16_t totalReadingDays =
        std::accumulate(monthlyReadingDays.begin(), monthlyReadingDays.end(), uint16_t{0});
    const uint8_t averageMonthlyReadingDays = static_cast<uint8_t>(
        (totalReadingDays + static_cast<uint16_t>(kReadingRhythmMonthCount / 2)) / kReadingRhythmMonthCount);

    const int monthlyY = chartY + weeklyChartH + frame.cardGap;
    drawStatsCardFrame(renderer, cardX, monthlyY, cardW, monthlyChartH, tr(STR_RHYTHM_DAYS_BY_MONTH), titleH);
    const int monthlyValueY = monthlyY + titleH + 4;
    const int monthlyFooterY = monthlyY + monthlyChartH - m.smallH - 4;
    const int monthlyLabelY = monthlyFooterY - m.smallH - 2;
    const int monthlyChartTop = monthlyValueY + m.smallH + 2;
    const int monthlyChartBottom = monthlyLabelY - 3;
    const int monthlyChartHeight = std::max(1, monthlyChartBottom - monthlyChartTop);
    const int monthlyGridX = cardX + kCardPadding;
    const int monthlyColumnStride =
        std::max(1, (cardW - kCardPadding * 2) / static_cast<int>(kReadingRhythmMonthCount));
    const int monthlyBarWidth = std::min(monthlyColumnStride - 6, compactWidth ? 14 : 20);
    renderer.drawLine(monthlyGridX, monthlyChartBottom,
                      monthlyGridX + monthlyColumnStride * static_cast<int>(kReadingRhythmMonthCount),
                      monthlyChartBottom);

    for (size_t monthIndex = 0; monthIndex < kReadingRhythmMonthCount; ++monthIndex) {
      const int columnX = monthlyGridX + static_cast<int>(monthIndex) * monthlyColumnStride;
      const uint8_t readingDays = monthlyReadingDays[monthIndex];
      char valueLabel[4];
      snprintf(valueLabel, sizeof(valueLabel), readingDays == 0 ? "-" : "%u", static_cast<unsigned>(readingDays));
      drawCenteredLabel(renderer, SMALL_FONT_ID, columnX, monthlyColumnStride, monthlyValueY, valueLabel);
      if (readingDays > 0) {
        const int barHeight =
            std::max(2, static_cast<int>((static_cast<uint32_t>(monthlyChartHeight) * readingDays) / 31u));
        const int barY = monthlyChartBottom - std::min(barHeight, monthlyChartHeight);
        renderer.fillRoundedRect(columnX + (monthlyColumnStride - monthlyBarWidth) / 2, barY, monthlyBarWidth,
                                 monthlyChartBottom - barY, 2, Color::DarkGray);
      }
      char monthLabel[8];
      formatReadingStatsMonthToken(monthDates[monthIndex], monthLabel, sizeof(monthLabel));
      drawCenteredLabel(renderer, SMALL_FONT_ID, columnX, monthlyColumnStride, monthlyLabelY, monthLabel);
    }

    char totalValue[20];
    snprintf(totalValue, sizeof(totalValue), "%u %s", static_cast<unsigned>(totalReadingDays),
             dayCountText(totalReadingDays));
    const char* totalText = tr(STR_RHYTHM_MONTHLY_TOTAL);
    renderer.drawText(SMALL_FONT_ID, cardX + kCardPadding, monthlyFooterY, totalText);
    renderer.drawText(SMALL_FONT_ID, cardX + kCardPadding + renderer.getTextWidth(SMALL_FONT_ID, totalText) + 4,
                      monthlyFooterY, totalValue);
    char monthlyAverageValue[20];
    snprintf(monthlyAverageValue, sizeof(monthlyAverageValue), "%u %s",
             static_cast<unsigned>(averageMonthlyReadingDays), dayCountText(averageMonthlyReadingDays));
    const char* monthlyAverageText = tr(STR_RHYTHM_MONTHLY_AVERAGE);
    const int monthlyAverageW = renderer.getTextWidth(SMALL_FONT_ID, monthlyAverageText);
    const int monthlyAverageX =
        cardX + cardW - kCardPadding - monthlyAverageW - 4 - renderer.getTextWidth(SMALL_FONT_ID, monthlyAverageValue);
    renderer.drawText(SMALL_FONT_ID, monthlyAverageX, monthlyFooterY, monthlyAverageText);
    renderer.drawText(SMALL_FONT_ID, monthlyAverageX + monthlyAverageW + 4, monthlyFooterY, monthlyAverageValue);
  }

  drawButtonHints(renderer, mappedInput, false, showMoreButton ? tr(STR_MORE) : "");
}

std::vector<uint16_t> paginateFinishedBooks(const GfxRenderer& renderer, const MappedInputManager* mappedInput,
                                            const std::vector<FinishedBookEntry>& finishedBooks) {
  const FinishedListMetrics m = finishedListMetrics(renderer, mappedInput);
  std::vector<uint16_t> starts;
  starts.reserve(4);
  starts.push_back(0);
  int y = m.listTop;
  size_t pageStart = 0;
  for (size_t i = 0; i < finishedBooks.size(); ++i) {
    const bool header = startsMonthSection(finishedBooks, i, pageStart);
    const int needed = (header ? m.headerRowH + (i > pageStart ? m.headerGap : 0) : 0) + m.bookRowH;
    if (y + needed > m.listBottom && i > pageStart) {
      pageStart = i;
      starts.push_back(static_cast<uint16_t>(i));
      y = m.listTop + m.headerRowH + m.bookRowH;  // a new page always opens with a month header
      continue;
    }
    y += needed;
  }
  return starts;
}

void renderFinishedBooksPage(GfxRenderer& renderer, const MappedInputManager* mappedInput,
                             const std::vector<FinishedBookEntry>& finishedBooks, const uint32_t completedBooks,
                             const std::vector<uint16_t>& pageStarts, const size_t listPage) {
  const FinishedListMetrics m = finishedListMetrics(renderer, mappedInput);
  const StatsPageFrame& frame = m.frame;
  drawStatsPageHeader(renderer, mappedInput, tr(STR_BOOKS_FINISHED));

  // ---- Totals card, laid out like the "All Books" card.
  uint32_t finishedSeconds = 0;
  for (const auto& entry : finishedBooks) finishedSeconds += entry.totalReadingSeconds;
  const uint32_t bookCount = std::max<uint32_t>(completedBooks, static_cast<uint32_t>(finishedBooks.size()));
  drawStatsCardFrame(renderer, frame.cardX, frame.top, frame.cardW, m.summaryH, tr(STR_FINISHED_TOTALS), frame.titleH);
  const int thirdW = frame.cardW / 3;
  const int cellY = frame.top + frame.titleH;
  const int cellH = m.summaryH - frame.titleH;
  char buf[32];
  snprintf(buf, sizeof(buf), bookCount > 0 ? "%lu" : "-", static_cast<unsigned long>(bookCount));
  drawStatsValueCell(renderer, frame.cardX, thirdW, cellY, cellH, buf, tr(STR_STATS_COMPLETED_LBL));
  formatRhythmMinutes(finishedSeconds / 60u, buf, sizeof(buf));
  drawStatsValueCell(renderer, frame.cardX + thirdW, thirdW, cellY, cellH, finishedBooks.empty() ? "-" : buf,
                     tr(STR_STATS_TIME_LBL));
  formatRhythmMinutes(finishedBooks.empty() ? 0 : finishedSeconds / 60u / finishedBooks.size(), buf, sizeof(buf));
  drawStatsValueCell(renderer, frame.cardX + thirdW * 2, frame.cardW - thirdW * 2, cellY, cellH,
                     finishedBooks.empty() ? "-" : buf, tr(STR_FINISHED_AVG_PER_BOOK));

  // ---- Finished list card, filling the rest of the frame.
  const size_t pageCount = std::max<size_t>(1, pageStarts.size());
  const size_t page = std::min(listPage, pageCount - 1);
  const int listCardY = frame.top + m.summaryH + frame.cardGap;
  const int listCardH = frame.bottom - listCardY;
  drawStatsCardFrame(renderer, frame.cardX, listCardY, frame.cardW, listCardH, tr(STR_FINISHED_RECENT), frame.titleH);
  const int smallH = renderer.getLineHeight(SMALL_FONT_ID);
  if (pageCount > 1) {
    char pageLabel[12];
    snprintf(pageLabel, sizeof(pageLabel), "%u/%u", static_cast<unsigned>(page + 1), static_cast<unsigned>(pageCount));
    renderer.drawText(SMALL_FONT_ID,
                      frame.cardX + frame.cardW - kCardPadding - renderer.getTextWidth(SMALL_FONT_ID, pageLabel),
                      listCardY + (frame.titleH - smallH) / 2, pageLabel);
  }

  if (finishedBooks.empty()) {
    drawCenteredLabel(renderer, UI_10_FONT_ID, frame.cardX, frame.cardW,
                      listCardY + frame.titleH + (listCardH - frame.titleH - renderer.getLineHeight(UI_10_FONT_ID)) / 2,
                      tr(STR_FINISHED_NONE));
  } else {
    const size_t first = pageStarts.empty() ? 0 : pageStarts[page];
    const size_t last = page + 1 < pageStarts.size() ? pageStarts[page + 1] : finishedBooks.size();
    const int textX = frame.cardX + kCardPadding;
    const int textW = frame.cardW - kCardPadding * 2;
    const int bodyH = renderer.getLineHeight(UI_10_FONT_ID);
    int y = m.listTop;
    for (size_t i = first; i < last; ++i) {
      const FinishedBookEntry& entry = finishedBooks[i];
      if (startsMonthSection(finishedBooks, i, first)) {
        if (i > first) y += m.headerGap;
        char header[64];
        formatMonthHeader(finishedBooks, entry.finishedDate, header, sizeof(header));
        const auto headerText = renderer.truncatedText(SMALL_FONT_ID, header, textW, EpdFontFamily::BOLD);
        renderer.drawText(SMALL_FONT_ID, textX, y + 3, headerText.c_str(), true, EpdFontFamily::BOLD);
        renderer.drawLine(textX, y + m.headerRowH - 3, textX + textW, y + m.headerRowH - 3);
        y += m.headerRowH;
      }

      // Line 1: title, with the time spent right-aligned.
      char duration[24];
      formatRhythmMinutes(entry.totalReadingSeconds / 60u, duration, sizeof(duration));
      const int durationW = renderer.getTextWidth(SMALL_FONT_ID, duration);
      const auto title =
          renderer.truncatedText(UI_10_FONT_ID, entry.title.c_str(), textW - durationW - 12, EpdFontFamily::BOLD);
      renderer.drawText(UI_10_FONT_ID, textX, y + 4, title.c_str(), true, EpdFontFamily::BOLD);
      renderer.drawText(SMALL_FONT_ID, textX + textW - durationW, y + 4 + (bodyH - smallH) / 2, duration);

      // Line 2: start -> finish dates, then the author.
      char finishDate[20];
      char dates[48];
      formatReadingStatsShortDate(entry.finishedDate, finishDate, sizeof(finishDate));
      if (entry.startDate.isValid()) {
        char startDate[20];
        formatReadingStatsShortDate(entry.startDate, startDate, sizeof(startDate));
        snprintf(dates, sizeof(dates), tr(STR_FINISHED_DATES_FMT), startDate, finishDate);
      } else {
        snprintf(dates, sizeof(dates), "%s", finishDate);
      }
      std::string detail = dates;
      if (!entry.author.empty()) detail += " - " + entry.author;
      const auto detailText = renderer.truncatedText(SMALL_FONT_ID, detail.c_str(), textW);
      renderer.drawText(SMALL_FONT_ID, textX, y + 6 + bodyH, detailText.c_str());
      y += m.bookRowH;
    }
  }

  drawButtonHints(renderer, mappedInput, page > 0, page + 1 < pageCount ? tr(STR_RHYTHM_NEXT_SHORT) : "");
}

}  // namespace RhythmStatsView
