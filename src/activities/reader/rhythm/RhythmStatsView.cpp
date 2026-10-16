// Ported from XPoint (https://github.com/Belphemur/XPoint, MIT):
// src/activities/reader/BookStatsView.cpp renderReadingRhythmPage() and
// renderFinishedBooksPage() plus their layout helpers. Layout constants and
// chart math are unchanged; see RhythmStatsView.h for the CrossInk changes.
#include "RhythmStatsView.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <numeric>

#include "MappedInputManager.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/ReadingStatsUtils.h"
#include "components/CompactHeader.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr int kStatsButtonHintTopGap = 10;
constexpr int kDefaultHeaderHeight = 78;
constexpr int kSectionTitleH = 30;
constexpr int kReadingRhythmWeekCount = 13;
constexpr size_t kReadingRhythmMonthCount = 12;
constexpr int kRecentReadingDayCount = 7;
constexpr uint16_t kMediumReadingMinutes = 15;
constexpr uint16_t kHighReadingMinutes = 30;

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

bool sameFinishedBookMonth(const FinishedBookEntry& entry, const ReadingStatsDate& date) {
  return entry.finishedDate.isValid() && date.isValid() && entry.finishedDate.year == date.year &&
         entry.finishedDate.month == date.month;
}

void formatRhythmMinutes(const uint32_t minutes, char* buf, const size_t len) {
  if (!buf || len == 0) return;
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

void formatFinishedBookMonthHeader(const ReadingStatsDate& date, const uint8_t bookCount, const uint32_t readingSeconds,
                                   char* buf, const size_t len) {
  if (!buf || len == 0) return;
  if (!date.isValid()) {
    snprintf(buf, len, "-");
    return;
  }
  char month[8];
  char duration[24];
  formatReadingStatsMonthToken(date, month, sizeof(month));
  formatRhythmMinutes(readingSeconds / 60u, duration, sizeof(duration));
  snprintf(buf, len, tr(STR_FINISHED_MONTH_FORMAT), month, static_cast<unsigned>(date.year),
           static_cast<unsigned>(bookCount), bookCount == 1 ? tr(STR_FINISHED_BOOK_ONE) : tr(STR_FINISHED_BOOK_MANY),
           duration);
}

void finishedBookMonthSummary(const std::vector<FinishedBookEntry>& finishedBooks, const ReadingStatsDate& date,
                              uint8_t& bookCount, uint32_t& readingSeconds) {
  bookCount = 0;
  readingSeconds = 0;
  for (const auto& entry : finishedBooks) {
    if (sameFinishedBookMonth(entry, date)) {
      if (bookCount < 255) ++bookCount;
      readingSeconds += entry.totalReadingSeconds;
    }
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

bool hasTouch(const MappedInputManager& input) { return input.hasTouchHardware(); }

int statsBottomInset(const ThemeMetrics& metrics, const MappedInputManager& input) {
  return metrics.verticalSpacing + (hasTouch(input) ? 0 : metrics.buttonHintsHeight + kStatsButtonHintTopGap);
}

int headerBottom(const ThemeMetrics& metrics, const MappedInputManager& input) {
  const int height =
      hasTouch(input) ? CompactHeader::height(metrics) : std::min(metrics.headerHeight, kDefaultHeaderHeight);
  return metrics.topPadding + height;
}

void drawScreenHeader(GfxRenderer& renderer, const MappedInputManager& input, const char* title) {
  if (hasTouch(input)) {
    TouchHeaderBackButton::drawCompact(renderer, title, false);
  } else {
    CompactHeader::drawTitle(renderer, title);
  }
}

void drawCenteredLabel(const GfxRenderer& renderer, const int fontId, const int x, const int w, const int y,
                       const char* text, const bool bold = false) {
  const int textWidth = renderer.getTextWidth(fontId, text, bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  renderer.drawText(fontId, x + (w - textWidth) / 2, y, text, true,
                    bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
}

void drawSectionCard(const GfxRenderer& renderer, const int x, const int y, const int w, const int h,
                     const char* title) {
  renderer.drawRect(x, y, w, h);
  renderer.drawLine(x, y + kSectionTitleH, x + w, y + kSectionTitleH);
  drawCenteredLabel(renderer, UI_10_FONT_ID, x, w, y + (kSectionTitleH - renderer.getLineHeight(UI_10_FONT_ID)) / 2,
                    title, true);
}

}  // namespace

namespace RhythmStatsView {

void renderReadingRhythmPage(GfxRenderer& renderer, const MappedInputManager& mappedInput,
                             const GlobalReadingStats& stats, const DailyMinutesLog& minutesLog) {
  renderer.clearScreen();
  drawScreenHeader(renderer, mappedInput, tr(STR_READING_RHYTHM));

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int screenW = renderer.getScreenWidth();
  const int cardX = metrics.contentSidePadding;
  const int cardW = screenW - metrics.contentSidePadding * 2;
  const bool compactWidth = screenW < 500;
  const int titleH = compactWidth ? 34 : 40;
  const int cardPadding = compactWidth ? 10 : 14;
  const int weekdayLabelW = compactWidth ? 38 : 46;
  const int monthLabelH = 25;
  const int rowStride = compactWidth ? 24 : 27;
  const int activeDotSize = compactWidth ? 12 : 14;
  const int inactiveDotSize = compactWidth ? 5 : 6;
  const int recentDaysH = 42;
  const int chartTitleH = kSectionTitleH;
  const int rhythmCardH = titleH + monthLabelH + rowStride * 7 + recentDaysH + cardPadding;
  const int chartGap = compactWidth ? 10 : 18;
  const int weeklyChartH = chartTitleH + (compactWidth ? 86 : 98);
  const int monthlyChartH = weeklyChartH;
  const int contentTop = headerBottom(metrics, mappedInput);
  const int availableH = renderer.getScreenHeight() - contentTop - statsBottomInset(metrics, mappedInput);
  const int baseContentH = rhythmCardH + chartGap + weeklyChartH;
  const int fullContentH = baseContentH + chartGap + monthlyChartH;
  const bool showMonthlyChart = availableH >= fullContentH + 12;
  const int contentH = showMonthlyChart ? fullContentH : baseContentH;
  const int rhythmY = contentTop + std::max(6, (availableH - contentH) / 2);

  renderer.drawRect(cardX, rhythmY, cardW, rhythmCardH);
  renderer.drawLine(cardX, rhythmY + titleH, cardX + cardW, rhythmY + titleH);
  drawCenteredLabel(renderer, UI_10_FONT_ID, cardX, cardW,
                    rhythmY + (titleH - renderer.getLineHeight(UI_10_FONT_ID)) / 2, tr(STR_RHYTHM_RECENT_ACTIVITY),
                    true);

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

  const int gridX = cardX + cardPadding + weekdayLabelW;
  const int gridW = cardW - cardPadding * 2 - weekdayLabelW;
  const int columnStride = std::max(1, gridW / kReadingRhythmWeekCount);
  const int gridTop = rhythmY + titleH + monthLabelH;
  uint8_t previousMonth = 0;
  bool anyEstimated = false;

  for (int week = 0; week < kReadingRhythmWeekCount; ++week) {
    const uint32_t weekStartDay = firstDay + static_cast<uint32_t>(week) * 7u;
    ReadingStatsDate weekDate;
    if (readingStatsDateFromDayIndex(weekStartDay, weekDate) && weekDate.month != previousMonth) {
      char monthToken[8];
      formatReadingStatsMonthToken(weekDate, monthToken, sizeof(monthToken));
      renderer.drawText(SMALL_FONT_ID, gridX + week * columnStride, rhythmY + titleH + 3, monthToken);
      previousMonth = weekDate.month;
    }
  }

  for (int weekday = 0; weekday < 7; ++weekday) {
    const int rowCenterY = gridTop + weekday * rowStride + rowStride / 2;
    const char* weekdayText = I18n::getInstance().get(DAY_LABELS[weekday]);
    renderer.drawText(SMALL_FONT_ID, cardX + cardPadding, rowCenterY - renderer.getLineHeight(SMALL_FONT_ID) / 2,
                      weekdayText);

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
        // Estimated days: same size as a real day, drawn as an outline.
        anyEstimated = true;
        renderer.drawRoundedRect(dotX, dotY, dotSize, dotSize, 1, dotSize / 2, true);
      } else {
        renderer.fillRoundedRect(dotX, dotY, dotSize, dotSize, dotSize / 2, dotColor);
      }

      if (hasToday && dayIndex == displayDay) {
        constexpr int todayRingGap = 3;
        const int ringSize = activeDotSize + todayRingGap * 2;
        renderer.drawRoundedRect(centerX - ringSize / 2, rowCenterY - ringSize / 2, ringSize, ringSize, 2, ringSize / 2,
                                 true);
      }
    }
  }

  const int recentDaysY = gridTop + rowStride * 7;
  renderer.drawLine(cardX, recentDaysY, cardX + cardW, recentDaysY);
  const int recentDayW = std::max(1, (cardW - cardPadding * 2) / kRecentReadingDayCount);
  const int recentDayLabelY = recentDaysY + 5;
  const int recentDayValueY = recentDayLabelY + renderer.getLineHeight(SMALL_FONT_ID) + 2;
  const uint32_t firstRecentDay =
      displayDay >= kRecentReadingDayCount - 1 ? displayDay - static_cast<uint32_t>(kRecentReadingDayCount - 1) : 0;
  for (int dayOffset = 0; dayOffset < kRecentReadingDayCount; ++dayOffset) {
    const uint32_t dayIndex = firstRecentDay + static_cast<uint32_t>(dayOffset);
    const int columnX = cardX + cardPadding + dayOffset * recentDayW;
    ReadingStatsDate date;
    if (!readingStatsDateFromDayIndex(dayIndex, date)) continue;

    if (hasToday && dayIndex == displayDay) {
      renderer.drawRoundedRect(columnX + 2, recentDaysY + 3, recentDayW - 4, recentDaysH - 6, 1, 4, true);
    }
    drawCenteredLabel(renderer, SMALL_FONT_ID, columnX, recentDayW, recentDayLabelY,
                      I18n::getInstance().get(DAY_LABELS[readingStatsDayOfWeekIndex(date)]));

    const DayMinutes day = minutesOnDay(stats, minutesLog, dayIndex);
    char duration[16];
    formatRhythmMinutes(day.minutes, duration, sizeof(duration));
    char value[20];
    snprintf(value, sizeof(value), "%s%s", day.estimated ? "~" : "", duration);
    drawCenteredLabel(renderer, SMALL_FONT_ID, columnX, recentDayW, recentDayValueY, value);
  }

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
  const uint32_t currentWeekMinutes = weeklyMinutes.back();

  const int chartY = rhythmY + rhythmCardH + chartGap;
  drawSectionCard(renderer, cardX, chartY, cardW, weeklyChartH, tr(STR_RHYTHM_WEEKLY_TIME));
  const int chartTop = chartY + chartTitleH + (compactWidth ? 7 : 9);
  const int footerH = renderer.getLineHeight(SMALL_FONT_ID) + 6;
  const int chartBottom = chartY + weeklyChartH - footerH - 5;
  const int chartHeight = std::max(1, chartBottom - chartTop);
  const int chartRight = gridX + columnStride * kReadingRhythmWeekCount;

  {
    char scaleLabel[16];
    formatRhythmMinutes(chartScaleMinutes, scaleLabel, sizeof(scaleLabel));
    renderer.drawText(SMALL_FONT_ID, cardX + cardPadding, chartTop, scaleLabel);
  }
  if (anyEstimated) {
    // Legend for the lighter estimated values, top-right of the weekly card.
    const char* note = tr(STR_RHYTHM_ESTIMATED_NOTE);
    renderer.drawText(SMALL_FONT_ID, cardX + cardW - cardPadding - renderer.getTextWidth(SMALL_FONT_ID, note), chartTop,
                      note);
  }
  renderer.drawLine(gridX, chartBottom, chartRight, chartBottom);

  if (averageWeeklyMinutes > 0) {
    const int averageY =
        chartBottom - static_cast<int>((static_cast<uint64_t>(chartHeight) * averageWeeklyMinutes) / chartScaleMinutes);
    for (int dashX = gridX; dashX < chartRight; dashX += 8) {
      renderer.drawLine(dashX, averageY, std::min(dashX + 3, chartRight), averageY);
    }
  }

  const int barWidth = compactWidth ? 10 : 14;
  for (int week = 0; week < kReadingRhythmWeekCount; ++week) {
    const uint32_t minutes = weeklyMinutes[static_cast<size_t>(week)];
    if (minutes == 0) continue;
    const int barHeight = std::min(
        chartHeight, std::max(2, static_cast<int>((static_cast<uint64_t>(chartHeight) * minutes) / chartScaleMinutes)));
    const uint32_t realMinutes = minutes - weeklyEstimatedMinutes[static_cast<size_t>(week)];
    const int realHeight =
        realMinutes == 0 ? 0 : std::max(2, static_cast<int>((static_cast<int64_t>(barHeight) * realMinutes) / minutes));
    const int barX = gridX + week * columnStride + columnStride / 2 - barWidth / 2;
    const int realTop = chartBottom - realHeight;
    if (realHeight > 0) {
      if (week == kReadingRhythmWeekCount - 1) {
        renderer.fillRect(barX, realTop, barWidth, realHeight);
      } else {
        renderer.fillRectDither(barX, realTop, barWidth, realHeight, Color::DarkGray);
      }
    }
    // Estimated share sits on top of the real share: outlined light segment.
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
  formatRhythmMinutes(currentWeekMinutes, currentValue, sizeof(currentValue));
  const int footerY = chartY + weeklyChartH - footerH + 2;
  const char* averageText = tr(STR_RHYTHM_WEEKLY_AVERAGE);
  const int averageX = cardX + cardPadding;
  renderer.drawText(SMALL_FONT_ID, averageX, footerY, averageText);
  renderer.drawText(SMALL_FONT_ID, averageX + renderer.getTextWidth(SMALL_FONT_ID, averageText) + 4, footerY,
                    averageValue);

  const char* currentText = tr(STR_RHYTHM_THIS_WEEK);
  const int currentTextW = renderer.getTextWidth(SMALL_FONT_ID, currentText);
  const int currentValueW = renderer.getTextWidth(SMALL_FONT_ID, currentValue);
  const int currentX = cardX + cardW - cardPadding - currentTextW - 4 - currentValueW;
  renderer.drawText(SMALL_FONT_ID, currentX, footerY, currentText);
  renderer.drawText(SMALL_FONT_ID, currentX + currentTextW + 4, footerY, currentValue);

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

    const int monthlyY = chartY + weeklyChartH + chartGap;
    drawSectionCard(renderer, cardX, monthlyY, cardW, monthlyChartH, tr(STR_RHYTHM_DAYS_BY_MONTH));

    const int monthlyLineH = renderer.getLineHeight(SMALL_FONT_ID);
    const int monthlyValueY = monthlyY + chartTitleH + 3;
    const int monthlyFooterY = monthlyY + monthlyChartH - monthlyLineH - 3;
    const int monthlyLabelY = monthlyFooterY - monthlyLineH;
    const int monthlyChartTop = monthlyValueY + monthlyLineH + 2;
    const int monthlyChartBottom = monthlyLabelY - 3;
    const int monthlyChartHeight = std::max(1, monthlyChartBottom - monthlyChartTop);
    const int monthlyGridX = cardX + cardPadding;
    const int monthlyGridW = cardW - cardPadding * 2;
    const int monthlyColumnStride = std::max(1, monthlyGridW / static_cast<int>(kReadingRhythmMonthCount));
    const int monthlyRight = monthlyGridX + monthlyColumnStride * static_cast<int>(kReadingRhythmMonthCount);
    const int monthlyBarWidth = compactWidth ? 9 : 13;

    renderer.drawLine(monthlyGridX, monthlyChartBottom, monthlyRight, monthlyChartBottom);

    for (size_t monthIndex = 0; monthIndex < kReadingRhythmMonthCount; ++monthIndex) {
      const int columnX = monthlyGridX + static_cast<int>(monthIndex) * monthlyColumnStride;
      const int centerX = columnX + monthlyColumnStride / 2;
      const uint8_t readingDays = monthlyReadingDays[monthIndex];

      char valueLabel[4];
      if (readingDays == 0) {
        snprintf(valueLabel, sizeof(valueLabel), "-");
      } else {
        snprintf(valueLabel, sizeof(valueLabel), "%u", static_cast<unsigned>(readingDays));
      }
      drawCenteredLabel(renderer, SMALL_FONT_ID, columnX, monthlyColumnStride, monthlyValueY, valueLabel);

      if (readingDays > 0) {
        const int barHeight =
            std::max(2, static_cast<int>((static_cast<uint32_t>(monthlyChartHeight) * readingDays) / 31u));
        const int barY = monthlyChartBottom - std::min(barHeight, monthlyChartHeight);
        renderer.fillRoundedRect(centerX - monthlyBarWidth / 2, barY, monthlyBarWidth, monthlyChartBottom - barY, 2,
                                 Color::DarkGray);
      }

      char monthLabel[8];
      formatReadingStatsMonthToken(monthDates[monthIndex], monthLabel, sizeof(monthLabel));
      drawCenteredLabel(renderer, SMALL_FONT_ID, columnX, monthlyColumnStride, monthlyLabelY, monthLabel);
    }

    char totalValue[20];
    snprintf(totalValue, sizeof(totalValue), "%u %s", static_cast<unsigned>(totalReadingDays),
             dayCountText(totalReadingDays));
    const char* totalText = tr(STR_RHYTHM_MONTHLY_TOTAL);
    renderer.drawText(SMALL_FONT_ID, cardX + cardPadding, monthlyFooterY, totalText);
    renderer.drawText(SMALL_FONT_ID, cardX + cardPadding + renderer.getTextWidth(SMALL_FONT_ID, totalText) + 4,
                      monthlyFooterY, totalValue);

    char monthlyAverageValue[20];
    snprintf(monthlyAverageValue, sizeof(monthlyAverageValue), "%u %s",
             static_cast<unsigned>(averageMonthlyReadingDays), dayCountText(averageMonthlyReadingDays));
    const char* monthlyAverageText = tr(STR_RHYTHM_MONTHLY_AVERAGE);
    const int monthlyAverageW = renderer.getTextWidth(SMALL_FONT_ID, monthlyAverageText);
    const int monthlyAverageValueW = renderer.getTextWidth(SMALL_FONT_ID, monthlyAverageValue);
    const int monthlyAverageX = cardX + cardW - cardPadding - monthlyAverageW - 4 - monthlyAverageValueW;
    renderer.drawText(SMALL_FONT_ID, monthlyAverageX, monthlyFooterY, monthlyAverageText);
    renderer.drawText(SMALL_FONT_ID, monthlyAverageX + monthlyAverageW + 4, monthlyFooterY, monthlyAverageValue);
  }

  if (!hasTouch(mappedInput)) {
    const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_EXIT)), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
  }
}

void renderFinishedBooksPage(GfxRenderer& renderer, const MappedInputManager& mappedInput,
                             const std::vector<FinishedBookEntry>& finishedBooks, const GlobalReadingStats& globalStats,
                             const size_t pageIndex, const size_t pageCount) {
  renderer.clearScreen();
  drawScreenHeader(renderer, mappedInput, tr(STR_BOOKS_FINISHED));

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int cardX = metrics.contentSidePadding;
  const int cardW = screenW - metrics.contentSidePadding * 2;
  const int contentTop = headerBottom(metrics, mappedInput);
  const int buttonTop = screenH - statsBottomInset(metrics, mappedInput);
  const int totalCardY = contentTop + metrics.verticalSpacing;
  const int totalCardH =
      std::max(62, renderer.getLineHeight(UI_12_FONT_ID) + renderer.getLineHeight(SMALL_FONT_ID) + 20);

  renderer.drawRoundedRect(cardX, totalCardY, cardW, totalCardH, 2, 10, true);
  char duration[32];
  formatRhythmMinutes(globalStats.totalReadingSeconds / 60u, duration, sizeof(duration));
  renderer.drawCenteredText(UI_12_FONT_ID, totalCardY + 10, duration, true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, totalCardY + totalCardH - renderer.getLineHeight(SMALL_FONT_ID) - 8,
                            tr(STR_STATS_TOTAL_READING_TIME_LBL));

  const int listY = totalCardY + totalCardH + metrics.verticalSpacing;
  const int availableListH = std::max(0, buttonTop - listY);
  const size_t firstEntry = pageIndex * FINISHED_BOOKS_ENTRIES_PER_PAGE;
  const size_t remainingEntries = firstEntry < finishedBooks.size() ? finishedBooks.size() - firstEntry : 0;
  const int visibleRows = static_cast<int>(std::min(FINISHED_BOOKS_ENTRIES_PER_PAGE, remainingEntries));

  if (visibleRows <= 0) {
    const int emptyY = listY + std::max(0, (availableListH - renderer.getLineHeight(UI_12_FONT_ID) -
                                            renderer.getLineHeight(UI_10_FONT_ID) - 8) /
                                               2);
    renderer.drawCenteredText(UI_12_FONT_ID, emptyY, "0", true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, emptyY + renderer.getLineHeight(UI_12_FONT_ID) + 8,
                              tr(STR_BOOKS_FINISHED));
  } else {
    const int headerH = std::max(22, renderer.getLineHeight(SMALL_FONT_ID) + 8);
    const int titleLineH = renderer.getLineHeight(UI_10_FONT_ID);
    const int authorLineH = renderer.getLineHeight(SMALL_FONT_ID);
    const int datesLineH = renderer.getLineHeight(SMALL_FONT_ID);
    const int bookRowH = std::max(40, titleLineH + authorLineH + datesLineH + 10);
    int sectionCount = 0;
    for (int index = 0; index < visibleRows; ++index) {
      const auto& entry = finishedBooks[firstEntry + index];
      if (index == 0 || !sameFinishedBookMonth(entry, finishedBooks[firstEntry + index - 1].finishedDate)) {
        ++sectionCount;
      }
    }
    const int minContentH = sectionCount * headerH + visibleRows * bookRowH;
    const int extraListH = std::max(0, availableListH - minContentH);
    const int gap = visibleRows > 1 ? std::min(metrics.verticalSpacing / 2, extraListH / (visibleRows - 1)) : 0;
    const int textX = cardX + metrics.contentSidePadding;
    const int textW = cardW - metrics.contentSidePadding * 2;
    int y = listY;
    for (int index = 0; index < visibleRows; ++index) {
      const auto& entry = finishedBooks[firstEntry + index];
      if (index == 0 || !sameFinishedBookMonth(entry, finishedBooks[firstEntry + index - 1].finishedDate)) {
        uint8_t monthBooks = 0;
        uint32_t monthSeconds = 0;
        finishedBookMonthSummary(finishedBooks, entry.finishedDate, monthBooks, monthSeconds);
        char header[64];
        formatFinishedBookMonthHeader(entry.finishedDate, monthBooks, monthSeconds, header, sizeof(header));
        renderer.drawText(SMALL_FONT_ID, cardX, y + 4, header, true, EpdFontFamily::BOLD);
        y += headerH;
      }

      const auto title = renderer.truncatedText(UI_10_FONT_ID, entry.title.c_str(), textW, EpdFontFamily::BOLD);
      renderer.drawText(UI_10_FONT_ID, textX, y + 2, title.c_str(), true, EpdFontFamily::BOLD);

      if (!entry.author.empty()) {
        const auto author = renderer.truncatedText(SMALL_FONT_ID, entry.author.c_str(), textW);
        renderer.drawText(SMALL_FONT_ID, textX, y + titleLineH + 4, author.c_str());
      }

      char finishDate[20];
      char rowDuration[24];
      char dates[64];
      formatReadingStatsShortDate(entry.finishedDate, finishDate, sizeof(finishDate));
      formatRhythmMinutes(entry.totalReadingSeconds / 60u, rowDuration, sizeof(rowDuration));
      if (entry.startDate.isValid()) {
        char startDate[20];
        formatReadingStatsShortDate(entry.startDate, startDate, sizeof(startDate));
        snprintf(dates, sizeof(dates), tr(STR_FINISHED_DATES_FMT), startDate, finishDate, rowDuration);
      } else {
        snprintf(dates, sizeof(dates), tr(STR_FINISHED_DATE_ONLY_FMT), finishDate, rowDuration);
      }
      const int datesTop = entry.author.empty() ? titleLineH + 4 : titleLineH + authorLineH + 6;
      renderer.drawText(SMALL_FONT_ID, textX, y + datesTop, dates);
      y += bookRowH + gap;
    }
  }

  if (pageCount > 1) {
    char pageLabel[16];
    snprintf(pageLabel, sizeof(pageLabel), "%u/%u", static_cast<unsigned>(pageIndex + 1),
             static_cast<unsigned>(pageCount));
    const int labelY = buttonTop - renderer.getLineHeight(SMALL_FONT_ID);
    renderer.drawCenteredText(SMALL_FONT_ID, labelY, pageLabel);
  }

  if (!hasTouch(mappedInput)) {
    const bool hasPrevious = pageIndex > 0;
    const bool hasNext = pageIndex + 1 < pageCount;
    const auto labels =
        mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_EXIT)), "", hasPrevious ? tr(STR_RHYTHM_PREV_SHORT) : "",
                              hasNext ? tr(STR_RHYTHM_NEXT_SHORT) : "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
  }
}

}  // namespace RhythmStatsView
