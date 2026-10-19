#include "BookStatsActivity.h"

#include <I18n.h>

#include <algorithm>

#include "BookStatsView.h"
#include "MappedInputManager.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "rhythm/BookStatsScan.h"
#include "rhythm/DailyMinutesBackfill.h"
#include "rhythm/RhythmStatsView.h"
#include "util/InputReleaseGuard.h"

namespace {

void drawPageIndicators(const GfxRenderer& renderer, const int currentPage, const int totalPages) {
  if (totalPages <= 1) {
    return;
  }

  constexpr int kDotSize = 8;
  constexpr int kDotSpacing = 6;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int totalDotWidth = totalPages * kDotSize + (totalPages - 1) * kDotSpacing;
  const int dotsStartX = (renderer.getScreenWidth() - totalDotWidth) / 2;
  const int dotY = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing - 4;
  for (int pageIndex = 0; pageIndex < totalPages; ++pageIndex) {
    const int dotX = dotsStartX + pageIndex * (kDotSize + kDotSpacing);
    if (pageIndex == currentPage) {
      renderer.fillRect(dotX, dotY, kDotSize, kDotSize, true);
    } else {
      renderer.drawRect(dotX, dotY, kDotSize, kDotSize, true);
    }
  }
}

}  // namespace

BookStatsActivity::BookStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                     const std::string& bookCachePath, const BookReadingStats& stats,
                                     const float progressPercent, const bool hasEstimatedTimeLeft,
                                     const uint32_t estimatedTimeLeftSeconds, const GlobalReadingStats& globalStats,
                                     const bool returnToHomeOnExit)
    : Activity("BookStats", renderer, mappedInput),
      bookTitle(title),
      bookCachePath(bookCachePath),
      stats(stats),
      globalStats(globalStats),
      returnToHomeOnExit(returnToHomeOnExit),
      progressPercent(progressPercent),
      hasEstimatedTimeLeft(hasEstimatedTimeLeft),
      estimatedTimeLeftSeconds(estimatedTimeLeftSeconds) {}

BookStatsActivity::BookStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                     const std::string& bookCachePath, const BookReadingStats& stats,
                                     const float progressPercent, const bool hasEstimatedTimeLeft,
                                     const uint32_t estimatedTimeLeftSeconds, const GlobalReadingStats& globalStats,
                                     const GlobalReadingStats& allDevicesStats, const bool returnToHomeOnExit)
    : Activity("BookStats", renderer, mappedInput),
      bookTitle(title),
      bookCachePath(bookCachePath),
      stats(stats),
      globalStats(globalStats),
      allDevicesStats(allDevicesStats),
      showAllDevicesStats(true),
      returnToHomeOnExit(returnToHomeOnExit),
      progressPercent(progressPercent),
      hasEstimatedTimeLeft(hasEstimatedTimeLeft),
      estimatedTimeLeftSeconds(estimatedTimeLeftSeconds) {}

void BookStatsActivity::refreshAllDevicesStats() {
  if (showAllDevicesStats) {
    allDevicesStats = GlobalReadingStats::loadAggregated(globalStats);
  }
}

void BookStatsActivity::saveStats() {
  if (!didChangeStats || !hasEditableBook()) {
    return;
  }

  if (!stats.save(bookCachePath)) {
    LOG_ERR("BookStats", "Could not save book stats");
    return;
  }
  globalStats.save();
  // Books Finished sidecar; the reader refreshes the author when it next records.
  FinishedBooksIndex::record(bookCachePath, bookTitle, "", stats);
  refreshAllDevicesStats();
  didChangeStats = false;
}

void BookStatsActivity::beginDateEditing() {
  dateEditStatsSnapshot = stats;
  dateEditGlobalStatsSnapshot = globalStats;
  didChangeStatsBeforeDateEdit = didChangeStats;
  dateEditSnapshotValid = true;
  page = Page::EditDates;
  requestUpdate();
}

void BookStatsActivity::finishDateEditing(const bool saveChanges) {
  if (saveChanges) {
    saveStats();
  } else if (dateEditSnapshotValid) {
    stats = dateEditStatsSnapshot;
    globalStats = dateEditGlobalStatsSnapshot;
    didChangeStats = didChangeStatsBeforeDateEdit;
    setResult(ReadingStatsResult{didChangeStats});
  }

  dateEditSnapshotValid = false;
  page = Page::PerBook;
  requestUpdate();
}

void BookStatsActivity::cycleEditField() { selectedEditField = (selectedEditField + 1) % 6; }

ReadingStatsDate BookStatsActivity::defaultDateForField(const bool finishedField) const {
  if (finishedField && stats.finishedDate.isValid()) {
    return stats.finishedDate;
  }
  if (!finishedField && stats.startDate.isValid()) {
    return stats.startDate;
  }
  if (finishedField && stats.startDate.isValid()) {
    return stats.startDate;
  }
  if (!finishedField && stats.finishedDate.isValid()) {
    return stats.finishedDate;
  }

  ReadingStatsDateTime now;
  if (getCurrentLocalReadingStatsDateTime(now)) {
    return now.date;
  }
  return {2000, 1, 1};
}

void BookStatsActivity::applyCompletedState(const bool completed) {
  if (stats.isCompleted == completed) {
    return;
  }

  stats.isCompleted = completed;
  if (completed) {
    globalStats.completedBooks++;
    if (!stats.finishedDateManual && !stats.finishedDate.isValid()) {
      ReadingStatsDateTime now;
      if (getCurrentLocalReadingStatsDateTime(now)) {
        stats.finishedDate = now.date;
      }
    }
  } else if (globalStats.completedBooks > 0) {
    globalStats.completedBooks--;
  }
}

void BookStatsActivity::normalizeEditedDates(const bool editedFinishedField) {
  if (!stats.startDate.isValid() || !stats.finishedDate.isValid()) {
    return;
  }
  if (compareReadingStatsDate(stats.finishedDate, stats.startDate) >= 0) {
    return;
  }

  if (editedFinishedField) {
    stats.startDate = stats.finishedDate;
  } else {
    stats.finishedDate = stats.startDate;
  }
}

void BookStatsActivity::clearEditedDate(const bool finishedField) {
  ReadingStatsDate& date = finishedField ? stats.finishedDate : stats.startDate;
  date.clear();

  if (finishedField) {
    stats.finishedDateManual = false;
    applyCompletedState(false);
  } else {
    stats.startDateManual = false;
  }

  didChangeStats = true;
  setResult(ReadingStatsResult{true});
  requestUpdate();
}

bool BookStatsActivity::shouldClearDateOnAdjust(const ReadingStatsDate& date, const bool finishedField,
                                                const int fieldIndex, const int delta) const {
  if (!date.isValid()) {
    return false;
  }

  switch (fieldIndex) {
    case 0:
      return (date.month == 1 && delta < 0) || (date.month == 12 && delta > 0);
    case 1: {
      const uint8_t monthDays = daysInMonth(date.year, date.month);
      return (date.day == 1 && delta < 0) || (date.day == monthDays && delta > 0);
    }
    case 2:
      return (date.year == 2000 && delta < 0) || (date.year == 2099 && delta > 0);
    default:
      return false;
  }
}

void BookStatsActivity::adjustSelectedDateField(const int delta) {
  const bool finishedField = selectedEditField >= 3;
  ReadingStatsDate& date = finishedField ? stats.finishedDate : stats.startDate;
  const int fieldIndex = selectedEditField % 3;

  if (shouldClearDateOnAdjust(date, finishedField, fieldIndex, delta)) {
    clearEditedDate(finishedField);
    return;
  }

  if (!date.isValid()) {
    date = defaultDateForField(finishedField);
  }

  switch (fieldIndex) {
    case 0: {
      int month = static_cast<int>(date.month) + delta;
      while (month < 1) {
        month += 12;
      }
      while (month > 12) {
        month -= 12;
      }
      date.month = static_cast<uint8_t>(month);
      break;
    }
    case 1: {
      const int monthDays = daysInMonth(date.year, date.month);
      int day = static_cast<int>(date.day) + delta;
      while (day < 1) {
        day += monthDays;
      }
      while (day > monthDays) {
        day -= monthDays;
      }
      date.day = static_cast<uint8_t>(day);
      break;
    }
    case 2: {
      int year = static_cast<int>(date.year) + delta;
      if (year < 2000) {
        year = 2099;
      } else if (year > 2099) {
        year = 2000;
      }
      date.year = static_cast<uint16_t>(year);
      break;
    }
  }

  const uint8_t monthDays = daysInMonth(date.year, date.month);
  if (date.day > monthDays) {
    date.day = monthDays;
  }

  if (finishedField) {
    stats.finishedDateManual = true;
    applyCompletedState(true);
  } else {
    stats.startDateManual = true;
  }
  normalizeEditedDates(finishedField);

  didChangeStats = true;
  setResult(ReadingStatsResult{true});
  requestUpdate();
}

void BookStatsActivity::onEnter() {
  Activity::onEnter();
  ignoreInitialBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);
  ignoreInitialConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  ignoreInitialPowerRelease = mappedInput.isPressed(MappedInputManager::Button::Power);
  if (bookCachePath.empty()) page = Page::ThisDevice;
  previousOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  requestUpdate();
}

void BookStatsActivity::onExit() {
  saveStats();
  renderer.setOrientation(previousOrientation);
  Activity::onExit();
}

void BookStatsActivity::exitStatsActivity() {
  if (returnToHomeOnExit) {
    onGoHome();
    return;
  }

  finish();
}

void BookStatsActivity::showPage(const Page newPage) {
  if (newPage == Page::ReadingRhythm && !rhythmLoaded) {
    // One-time minutes backfill runs here; read days come from this screen's stats.
    rhythmMinutes =
        DailyMinutesBackfill::runOnceIfNeeded(globalStats.readingHistoryAnchorDay, globalStats.readingHistoryBits);
    rhythmLoaded = true;
  }
  if (newPage == Page::FinishedBooks) {
    // Reloaded on every visit so completion edits made on this screen show up.
    reloadFinishedBooks();
    finishedTop = 0;
    finishedSelected = mappedInput.hasTouchHardware() || finishedBooks.empty() ? -1 : 0;
  }
  page = newPage;
  requestUpdate();
}

void BookStatsActivity::reloadFinishedBooks() {
  finishedBooks = FinishedBooksIndex::load(globalStats.completedBooks);
  const size_t maxTop = RhythmStatsView::finishedMaxTop(renderer, &mappedInput, finishedBooks);
  finishedTop = std::min(finishedTop, maxTop);
  if (finishedSelected >= static_cast<int>(finishedBooks.size())) {
    finishedSelected = static_cast<int>(finishedBooks.size()) - 1;
  }
}

void BookStatsActivity::moveFinishedSelection(const int delta) {
  if (finishedBooks.empty()) return;
  const int last = static_cast<int>(finishedBooks.size()) - 1;
  const int next = finishedSelected < 0 ? static_cast<int>(finishedTop) : std::clamp(finishedSelected + delta, 0, last);
  finishedSelected = next;
  // Keep the selection inside the scroll panel.
  if (static_cast<size_t>(next) < finishedTop) finishedTop = static_cast<size_t>(next);
  while (static_cast<size_t>(next) >=
         finishedTop + RhythmStatsView::finishedVisibleCount(renderer, &mappedInput, finishedBooks, finishedTop)) {
    ++finishedTop;
  }
  requestUpdate();
}

void BookStatsActivity::scrollFinishedList(const int direction) {
  if (finishedBooks.empty()) return;
  const size_t visible = RhythmStatsView::finishedVisibleCount(renderer, &mappedInput, finishedBooks, finishedTop);
  const size_t step = std::max<size_t>(1, visible > 1 ? visible - 1 : 1);
  const size_t maxTop = RhythmStatsView::finishedMaxTop(renderer, &mappedInput, finishedBooks);
  const size_t next =
      direction > 0 ? std::min(maxTop, finishedTop + step) : (finishedTop > step ? finishedTop - step : 0);
  if (next == finishedTop) return;
  finishedTop = next;
  if (finishedSelected >= 0) {
    const size_t nowVisible = RhythmStatsView::finishedVisibleCount(renderer, &mappedInput, finishedBooks, finishedTop);
    finishedSelected = static_cast<int>(
        std::clamp<size_t>(static_cast<size_t>(finishedSelected), finishedTop, finishedTop + nowVisible - 1));
  }
  requestUpdate();
}

void BookStatsActivity::openFinishedBook(const size_t index) {
  if (index >= finishedBooks.size()) return;
  const FinishedBookEntry& entry = finishedBooks[index];
  std::string cachePath;
  if (!BookStatsScan::findCachePathForKey(entry.pathKey, cachePath)) return;
  finishedSelected = static_cast<int>(index);
  const BookReadingStats bookStats = BookReadingStats::load(cachePath);
  startActivityForResult(std::make_unique<BookStatsActivity>(renderer, mappedInput, entry.title, cachePath, bookStats,
                                                             -1.0f, false, 0, globalStats),
                         [this](const ActivityResult&) {
                           // Date or completion edits for that book may have changed the list.
                           reloadFinishedBooks();
                           requestUpdate();
                         });
}

bool BookStatsActivity::showNextStatsPage() {
  switch (page) {
    case Page::PerBook:
      showPage(Page::ThisDevice);
      return true;
    case Page::ThisDevice:
      showPage(showAllDevicesStats ? Page::AllDevices : Page::ReadingRhythm);
      return true;
    case Page::AllDevices:
      showPage(Page::ReadingRhythm);
      return true;
    case Page::ReadingRhythm:
      showPage(Page::FinishedBooks);
      return true;
    case Page::FinishedBooks:
    case Page::EditDates:
      return false;
  }
  return false;
}

bool BookStatsActivity::showPreviousStatsPage() {
  switch (page) {
    case Page::FinishedBooks:
      showPage(Page::ReadingRhythm);
      return true;
    case Page::ReadingRhythm:
      showPage(showAllDevicesStats ? Page::AllDevices : Page::ThisDevice);
      return true;
    case Page::AllDevices:
      showPage(Page::ThisDevice);
      return true;
    case Page::ThisDevice:
      showPage(Page::PerBook);
      return true;
    case Page::PerBook:
    case Page::EditDates:
      return false;
  }
  return false;
}

int BookStatsActivity::pageIndicatorCount() const { return (showAllDevicesStats ? 3 : 2) + 2; }

int BookStatsActivity::pageIndicatorIndex() const {
  const int devicePages = showAllDevicesStats ? 3 : 2;
  switch (page) {
    case Page::ReadingRhythm:
      return devicePages;
    case Page::FinishedBooks:
      return devicePages + 1;
    default:
      return static_cast<int>(page);
  }
}

bool BookStatsActivity::selectEditFieldFromTouchTarget(const int touchTarget) {
  if (touchTarget < BookStatsTouchTarget::DateFieldBase ||
      touchTarget >= BookStatsTouchTarget::DateFieldBase + BookStatsTouchTarget::DateFieldCount) {
    return false;
  }

  const int newEditField = touchTarget - BookStatsTouchTarget::DateFieldBase;
  if (selectedEditField != newEditField) {
    selectedEditField = newEditField;
    requestUpdate();
  }
  return true;
}

void BookStatsActivity::loop() {
  if (InputReleaseGuard::consumeInitialRelease(mappedInput, MappedInputManager::Button::Back,
                                               ignoreInitialBackRelease) ||
      InputReleaseGuard::consumeInitialRelease(mappedInput, MappedInputManager::Button::Power,
                                               ignoreInitialPowerRelease) ||
      InputReleaseGuard::consumeInitialRelease(mappedInput, MappedInputManager::Button::Confirm,
                                               ignoreInitialConfirmRelease)) {
    return;
  }

  if (TouchHeaderBackButton::wasTapped(mappedInput, TouchHeaderBackButton::compactHeaderRect(renderer))) {
    if (page == Page::EditDates) {
      finishDateEditing(true);
    } else {
      exitStatsActivity();
    }
    return;
  }
  if (usesNoRtcSingleScreenLayout()) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      mappedInput.suppressNextBackRelease();
      exitStatsActivity();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      mappedInput.suppressNextConfirmRelease();
      exitStatsActivity();
      return;
    }
    return;
  }

  const bool upOrLeftPressed = mappedInput.wasPressed(MappedInputManager::Button::Up) ||
                               mappedInput.wasPressed(MappedInputManager::Button::Left);
  const bool downOrRightPressed = mappedInput.wasPressed(MappedInputManager::Button::Down) ||
                                  mappedInput.wasPressed(MappedInputManager::Button::Right);

  if (page == Page::EditDates) {
    int touchedTarget = -1;
    if (mappedInput.wasItemTouchedDown(touchedTarget) && selectEditFieldFromTouchTarget(touchedTarget)) {
      return;
    }
    int tappedTarget = -1;
    if (mappedInput.wasItemTapped(tappedTarget)) {
      if (selectEditFieldFromTouchTarget(tappedTarget)) {
        return;
      }
      if (tappedTarget == BookStatsTouchTarget::DateAdjustUp) {
        adjustSelectedDateField(1);
        return;
      }
      if (tappedTarget == BookStatsTouchTarget::DateAdjustDown) {
        adjustSelectedDateField(-1);
        return;
      }
      if (tappedTarget == BookStatsTouchTarget::DateSave) {
        finishDateEditing(true);
        return;
      }
      if (tappedTarget == BookStatsTouchTarget::DateCancel) {
        finishDateEditing(false);
        return;
      }
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finishDateEditing(true);
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      cycleEditField();
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
        mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      adjustSelectedDateField(-1);
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
        mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      adjustSelectedDateField(1);
      return;
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    mappedInput.suppressNextBackRelease();
    exitStatsActivity();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    mappedInput.suppressNextConfirmRelease();
    if (page == Page::PerBook && hasEditableBook()) {
      beginDateEditing();
      return;
    }
    if (page == Page::FinishedBooks && !finishedBooks.empty()) {
      // First press selects a row on touch devices; the next one opens it.
      if (finishedSelected < 0) {
        moveFinishedSelection(0);
      } else {
        openFinishedBook(static_cast<size_t>(finishedSelected));
      }
      return;
    }
    exitStatsActivity();
    return;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Left && showNextStatsPage()) {
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Right) {
    if (!showPreviousStatsPage()) {
      exitStatsActivity();
    }
    return;
  }

  if (page == Page::PerBook) {
    int touchedTarget = -1;
    if (hasEditableBook() && mappedInput.wasItemTapped(touchedTarget) &&
        touchedTarget == BookStatsTouchTarget::StartedDaysStat) {
      beginDateEditing();
      return;
    }
    if (hasEditableBook() && upOrLeftPressed) {
      beginDateEditing();
      return;
    }
    if (downOrRightPressed) {
      showNextStatsPage();
      return;
    }
    return;
  }

  if (page == Page::FinishedBooks) {
    int tappedTarget = -1;
    if (mappedInput.wasItemTapped(tappedTarget) && tappedTarget >= RhythmStatsView::FINISHED_ROW_TOUCH_BASE) {
      openFinishedBook(static_cast<size_t>(tappedTarget - RhythmStatsView::FINISHED_ROW_TOUCH_BASE));
      return;
    }
    // Vertical swipes scroll the list panel; Up/Down move the selection.
    if (swipe == MappedInputManager::SwipeDir::Up) {
      scrollFinishedList(1);
      return;
    }
    if (swipe == MappedInputManager::SwipeDir::Down) {
      scrollFinishedList(-1);
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      moveFinishedSelection(1);
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      // Past the first book, Up returns to the previous stats page as before.
      if (finishedSelected > 0) {
        moveFinishedSelection(-1);
        return;
      }
      showPreviousStatsPage();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) return;
  }

  if (upOrLeftPressed) {
    showPreviousStatsPage();
    return;
  }

  if (downOrRightPressed) {
    showNextStatsPage();
  }
}

void BookStatsActivity::render(RenderLock&&) {
  if (usesNoRtcSingleScreenLayout()) {
    renderNoRtcCombinedStatsPage(renderer, &mappedInput, bookTitle, stats, progressPercent, hasEstimatedTimeLeft,
                                 estimatedTimeLeftSeconds, globalStats,
                                 showAllDevicesStats ? &allDevicesStats : nullptr, true);
    renderer.displayBuffer();
    return;
  }

  switch (page) {
    case Page::PerBook:
      renderPerBookStatsPage(renderer, &mappedInput, bookTitle, stats, progressPercent, hasEstimatedTimeLeft,
                             estimatedTimeLeftSeconds, true, hasEditableBook(), true);
      break;
    case Page::ThisDevice:
      renderGlobalStatsPage(renderer, &mappedInput, tr(STR_STATS_THIS_DEVICE_SCREEN), globalStats, true, true);
      break;
    case Page::AllDevices:
      renderGlobalStatsPage(renderer, &mappedInput, tr(STR_STATS_ALL_DEVICES_SCREEN), allDevicesStats, true, true);
      break;
    case Page::ReadingRhythm:
      RhythmStatsView::renderReadingRhythmPage(renderer, &mappedInput, globalStats, rhythmMinutes, true);
      break;
    case Page::FinishedBooks:
      RhythmStatsView::renderFinishedBooksPage(renderer, &mappedInput, finishedBooks, globalStats.completedBooks,
                                               finishedTop, finishedSelected);
      break;
    case Page::EditDates:
      renderEditBookDatesPage(renderer, &mappedInput, bookTitle, stats, selectedEditField, true);
      break;
  }
  if (page != Page::EditDates) {
    drawPageIndicators(renderer, pageIndicatorIndex(), pageIndicatorCount());
  }
  renderer.displayBuffer();
}
