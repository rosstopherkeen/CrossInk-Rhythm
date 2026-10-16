#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "activities/reader/ReadingStatsUtils.h"

// Per-day reading minutes for the Reading Rhythm screen, persisted to the
// sidecar /.crosspoint/daily_minutes.bin.
//
// Ported from XPoint (https://github.com/Belphemur/XPoint, MIT), which keeps
// the same 91-day minute window inside global_stats.bin v6. CrossInk keeps
// global_stats.bin at v3 so stock firmware can still read it; the minutes live
// here instead and the read/not-read days still come from
// GlobalReadingStats::readingHistoryBits.
//
// Real minutes are recorded from the existing reading-session commit path.
// Estimated minutes come from a one-time backfill (DailyMinutesBackfill) and
// are kept in a separate array so a real recording always wins.
//
// Binary layout v1 (380 bytes, little-endian):
//   [0-3]     magic "CIDM"
//   [4]       version (= 1)
//   [5]       flags (bit 0 = backfill done)
//   [6-7]     window days (= 91)
//   [8-11]    anchorDay      uint32  day index of slot 0 (0 = empty log)
//   [12-15]   firstRealDay   uint32  first day with a real recording (0 = none yet)
//   [16-197]  realMinutes[91]      uint16 each, slot 0 = anchorDay, higher = older
//   [198-379] estimatedMinutes[91] uint16 each, same indexing
struct DailyMinutesLog {
  static constexpr size_t WINDOW_DAYS = 91;
  static constexpr uint16_t MAX_MINUTES_PER_DAY = 1440;
  static constexpr uint8_t FILE_VERSION = 1;
  static constexpr size_t FILE_SIZE = 16 + WINDOW_DAYS * 2 * 2;
  static constexpr uint8_t FLAG_BACKFILL_DONE = 0x01;

  uint8_t flags = 0;
  uint32_t anchorDay = 0;
  uint32_t firstRealDay = 0;
  std::array<uint16_t, WINDOW_DAYS> realMinutes{};
  std::array<uint16_t, WINDOW_DAYS> estimatedMinutes{};

  bool isEmpty() const { return anchorDay == 0; }
  void clear();
  bool backfillDone() const { return (flags & FLAG_BACKFILL_DONE) != 0; }

  // Moves the window so dayIndex becomes slot 0 when it is newer than the
  // current anchor. Older days fall off the end of both arrays.
  void advanceTo(uint32_t dayIndex);

  // Splits a reading span at local midnight and adds the rounded minutes of
  // each segment to that day's real minutes (same rule as XPoint).
  void recordSpan(const ReadingStatsDateTime& localStart, uint32_t seconds);

  // Adds estimated minutes to a day inside the window, clamped per day.
  void addEstimatedMinutes(uint32_t dayIndex, uint32_t minutes);

  // Display minutes for a day: real minutes when present, otherwise the
  // estimate. estimated is set when the estimate was used.
  uint16_t minutesOnDay(uint32_t dayIndex, bool* estimated = nullptr) const;
  uint16_t realMinutesOnDay(uint32_t dayIndex) const;
  uint16_t estimatedMinutesOnDay(uint32_t dayIndex) const;

  void serialize(uint8_t* out) const;
  // Returns false (and leaves *this untouched) when data is not a v1 file.
  // newerVersion is set when the magic matches but the version is unknown.
  bool deserialize(const uint8_t* data, size_t size, bool* newerVersion = nullptr);

  // Loads /.crosspoint/daily_minutes.bin, falling back to the .bak file.
  // Returns an empty log when neither file is usable.
  static DailyMinutesLog load();
  // Same as load() but decodes into out; returns false when out was reset.
  static bool loadInto(DailyMinutesLog& out);
  // Atomically writes /.crosspoint/daily_minutes.bin. Refuses to overwrite a
  // file written by a newer version of this format.
  bool save() const;

  // Session-commit hook: load, record, save. Runs once per reading session.
  static void recordSpanAndSave(const ReadingStatsDateTime& localStart, uint32_t seconds);

#ifdef READING_RHYTHM_TEST
  static void resetForwardGuardForTest();
#endif

 private:
  bool slotForDay(uint32_t dayIndex, size_t& slot) const;
};
