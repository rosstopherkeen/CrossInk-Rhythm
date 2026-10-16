// Ported from XPoint (https://github.com/Belphemur/XPoint, MIT):
// src/activities/reader/ReadingRhythmActivity.cpp.
#include "ReadingRhythmActivity.h"

#include <GfxRenderer.h>

#include "DailyMinutesBackfill.h"
#include "MappedInputManager.h"
#include "RhythmStatsView.h"
#include "components/TouchHeaderBackButton.h"
#include "util/InputReleaseGuard.h"

ReadingRhythmActivity::ReadingRhythmActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("ReadingRhythm", renderer, mappedInput) {}

void ReadingRhythmActivity::onEnter() {
  Activity::onEnter();
  ignoreInitialBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);
  ignoreInitialConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  // Read-only: global_stats.bin is loaded for its read-day bits, never saved here.
  globalStats = GlobalReadingStats::load();
  minutes = DailyMinutesBackfill::runOnceIfNeeded(globalStats.readingHistoryAnchorDay, globalStats.readingHistoryBits);
  previousOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  requestUpdate();
}

void ReadingRhythmActivity::onExit() {
  renderer.setOrientation(previousOrientation);
  Activity::onExit();
}

void ReadingRhythmActivity::loop() {
  if (InputReleaseGuard::consumeInitialRelease(mappedInput, MappedInputManager::Button::Back,
                                               ignoreInitialBackRelease) ||
      InputReleaseGuard::consumeInitialRelease(mappedInput, MappedInputManager::Button::Confirm,
                                               ignoreInitialConfirmRelease)) {
    return;
  }
  if (TouchHeaderBackButton::wasTapped(mappedInput, TouchHeaderBackButton::compactHeaderRect(renderer)) ||
      mappedInput.wasSwipe() == MappedInputManager::SwipeDir::Right) {
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
  }
}

void ReadingRhythmActivity::render(RenderLock&&) {
  RhythmStatsView::renderReadingRhythmPage(renderer, mappedInput, globalStats, minutes);
  renderer.displayBuffer();
}
