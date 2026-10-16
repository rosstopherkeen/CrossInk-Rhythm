// Ported from XPoint (https://github.com/Belphemur/XPoint, MIT):
// src/activities/reader/FinishedBooksActivity.cpp.
#include "FinishedBooksActivity.h"

#include <GfxRenderer.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "RhythmStatsView.h"
#include "components/TouchHeaderBackButton.h"
#include "util/InputReleaseGuard.h"

FinishedBooksActivity::FinishedBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("FinishedBooks", renderer, mappedInput) {}

size_t FinishedBooksActivity::pageCount() const {
  constexpr size_t perPage = RhythmStatsView::FINISHED_BOOKS_ENTRIES_PER_PAGE;
  return std::max<size_t>(1, (finishedBooks.size() + perPage - 1) / perPage);
}

bool FinishedBooksActivity::showPage(const size_t newPage) {
  if (newPage == page || newPage >= pageCount()) return false;
  page = newPage;
  requestUpdate();
  return true;
}

void FinishedBooksActivity::onEnter() {
  Activity::onEnter();
  ignoreInitialBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);
  ignoreInitialConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  // Read-only: global_stats.bin supplies the total time and completed count.
  globalStats = GlobalReadingStats::load();
  finishedBooks = FinishedBooksIndex::load(globalStats.completedBooks);
  previousOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  requestUpdate();
}

void FinishedBooksActivity::onExit() {
  renderer.setOrientation(previousOrientation);
  finishedBooks.clear();
  finishedBooks.shrink_to_fit();
  Activity::onExit();
}

void FinishedBooksActivity::loop() {
  if (InputReleaseGuard::consumeInitialRelease(mappedInput, MappedInputManager::Button::Back,
                                               ignoreInitialBackRelease) ||
      InputReleaseGuard::consumeInitialRelease(mappedInput, MappedInputManager::Button::Confirm,
                                               ignoreInitialConfirmRelease)) {
    return;
  }
  if (TouchHeaderBackButton::wasTapped(mappedInput, TouchHeaderBackButton::compactHeaderRect(renderer))) {
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    mappedInput.suppressNextBackRelease();
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    mappedInput.suppressNextConfirmRelease();
    finish();
    return;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Left) {
    showPage(page + 1);
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Right) {
    if (page == 0) {
      finish();
    } else {
      showPage(page - 1);
    }
    return;
  }

  if ((mappedInput.wasPressed(MappedInputManager::Button::Up) ||
       mappedInput.wasPressed(MappedInputManager::Button::Left)) &&
      page > 0) {
    showPage(page - 1);
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
      mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    showPage(page + 1);
  }
}

void FinishedBooksActivity::render(RenderLock&&) {
  RhythmStatsView::renderFinishedBooksPage(renderer, mappedInput, finishedBooks, globalStats, page, pageCount());
  renderer.displayBuffer();
}
