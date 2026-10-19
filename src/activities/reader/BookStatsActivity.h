#pragma once

#include <HalClock.h>

#include <string>
#include <vector>

#include "../Activity.h"
#include "BookReadingStats.h"
#include "GlobalReadingStats.h"
#include "rhythm/BookRhythm.h"
#include "rhythm/DailyMinutesLog.h"
#include "rhythm/FinishedBooksIndex.h"

class BookStatsActivity final : public Activity {
  enum class Page : uint8_t { PerBook, ThisDevice, AllDevices, EditDates, ReadingRhythm, FinishedBooks, BookRhythm };

  std::string bookTitle;
  std::string bookCachePath;
  BookReadingStats stats;
  GlobalReadingStats globalStats;
  GlobalReadingStats allDevicesStats;
  bool showAllDevicesStats = false;
  bool returnToHomeOnExit = false;
  float progressPercent = -1.0f;
  bool hasEstimatedTimeLeft = false;
  uint32_t estimatedTimeLeftSeconds = 0;
  GfxRenderer::Orientation previousOrientation = GfxRenderer::Orientation::Portrait;
  Page page = Page::PerBook;
  int selectedEditField = 0;
  bool didChangeStats = false;
  BookReadingStats dateEditStatsSnapshot;
  GlobalReadingStats dateEditGlobalStatsSnapshot;
  bool dateEditSnapshotValid = false;
  bool didChangeStatsBeforeDateEdit = false;
  bool ignoreInitialBackRelease = false;
  bool ignoreInitialConfirmRelease = false;
  bool ignoreInitialPowerRelease = false;
  // Reading Rhythm / Books Finished pages, loaded when first shown.
  DailyMinutesLog rhythmMinutes;
  bool rhythmLoaded = false;
  // Book detail mode (opened from Books Finished): Overview + Book Rhythm only.
  bool bookDetailMode = false;
  BookRhythm bookRhythm;
  bool bookRhythmLoaded = false;
  std::vector<FinishedBookEntry> finishedBooks;
  size_t finishedTop = 0;     // first book shown in the scroll panel
  int finishedSelected = -1;  // outlined row; -1 until a button selects one

  bool hasEditableBook() const { return !bookCachePath.empty() && halClock.isAvailable(); }
  bool usesNoRtcSingleScreenLayout() const { return !halClock.isAvailable(); }
  void refreshAllDevicesStats();
  void saveStats();
  void beginDateEditing();
  void finishDateEditing(bool saveChanges);
  void cycleEditField();
  void adjustSelectedDateField(int delta);
  void applyCompletedState(bool completed);
  ReadingStatsDate defaultDateForField(bool finishedField) const;
  void clearEditedDate(bool finishedField);
  bool shouldClearDateOnAdjust(const ReadingStatsDate& date, bool finishedField, int fieldIndex, int delta) const;
  void normalizeEditedDates(const bool editedFinishedField);
  void exitStatsActivity();
  bool showPreviousStatsPage();
  bool showNextStatsPage();
  bool selectEditFieldFromTouchTarget(int touchTarget);
  void showPage(Page newPage);
  void reloadFinishedBooks();
  void moveFinishedSelection(int delta);
  void scrollFinishedList(int direction);
  void openFinishedBook(size_t index);
  int pageIndicatorIndex() const;
  int pageIndicatorCount() const;

 public:
  BookStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                    const std::string& bookCachePath, const BookReadingStats& stats, float progressPercent,
                    bool hasEstimatedTimeLeft, uint32_t estimatedTimeLeftSeconds, const GlobalReadingStats& globalStats,
                    bool returnToHomeOnExit = false);
  BookStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                    const std::string& bookCachePath, const BookReadingStats& stats, float progressPercent,
                    bool hasEstimatedTimeLeft, uint32_t estimatedTimeLeftSeconds, const GlobalReadingStats& globalStats,
                    const GlobalReadingStats& allDevicesStats, bool returnToHomeOnExit = false);

  // Restricts this screen to the book's own pages: Overview and Book Rhythm.
  void enableBookDetailMode() { bookDetailMode = true; }

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool allowPowerAsConfirmInReaderMode() const override { return true; }
};
