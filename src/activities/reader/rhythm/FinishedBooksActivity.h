#pragma once

#include <vector>

#include "FinishedBooksIndex.h"
#include "activities/Activity.h"
#include "activities/reader/GlobalReadingStats.h"

// Books Finished screen: total reading time plus a paged list of finished
// books with start/finish dates and time spent. Ported from XPoint
// (https://github.com/Belphemur/XPoint, MIT) FinishedBooksActivity.
class FinishedBooksActivity final : public Activity {
  GlobalReadingStats globalStats;
  std::vector<FinishedBookEntry> finishedBooks;
  size_t page = 0;
  GfxRenderer::Orientation previousOrientation = GfxRenderer::Orientation::Portrait;
  bool ignoreInitialBackRelease = false;
  bool ignoreInitialConfirmRelease = false;

  size_t pageCount() const;
  bool showPage(size_t newPage);

 public:
  FinishedBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
