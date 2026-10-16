#pragma once

#include "DailyMinutesLog.h"
#include "activities/Activity.h"
#include "activities/reader/GlobalReadingStats.h"

// Reading Rhythm screen: recent activity dot grid, weekly reading time bars
// and reading days by month. Ported from XPoint
// (https://github.com/Belphemur/XPoint, MIT) ReadingRhythmActivity.
class ReadingRhythmActivity final : public Activity {
  GlobalReadingStats globalStats;
  DailyMinutesLog minutes;
  GfxRenderer::Orientation previousOrientation = GfxRenderer::Orientation::Portrait;
  bool ignoreInitialBackRelease = false;
  bool ignoreInitialConfirmRelease = false;

 public:
  ReadingRhythmActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
