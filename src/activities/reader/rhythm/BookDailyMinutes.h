#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "activities/reader/ReadingStatsUtils.h"

// Real reading minutes per day for one book, persisted next to the book's
// stats in <cache>/rhythm_days.bin. stats_v5.bin is never touched; this file
// only feeds the per-book Reading Rhythm page.
//
// Binary layout v1 (little-endian):
//   [0-3]  magic "CIBR"
//   [4]    version (= 1)
//   [5]    flags (reserved, 0)
//   [6-7]  entry count (uint16, at most MAX_DAYS)
//   then per entry, sorted by day: uint32 dayIndex, uint16 minutes
struct BookDailyMinutes {
  struct Day {
    uint32_t day = 0;
    uint16_t minutes = 0;
  };

  static constexpr size_t MAX_DAYS = 400;
  static constexpr uint8_t FILE_VERSION = 1;
  static constexpr size_t HEADER_SIZE = 8;
  static constexpr size_t ENTRY_SIZE = 6;
  static constexpr uint16_t MAX_MINUTES_PER_DAY = 1440;

  std::vector<Day> days;  // ascending by day

  // Same rule as DailyMinutesLog::recordSpan: split at local midnight, round
  // each day's segment to the nearest minute, clamp at 1440 per day. Oldest
  // days are dropped beyond MAX_DAYS.
  void recordSpan(const ReadingStatsDateTime& localStart, uint32_t seconds);
  uint16_t minutesOn(uint32_t dayIndex) const;

  size_t encodedSize() const { return HEADER_SIZE + days.size() * ENTRY_SIZE; }
  void serialize(uint8_t* out) const;  // out must hold encodedSize() bytes
  // Returns false for anything that is not a complete v1 file.
  // newerVersion is set when the magic matches but the version is unknown.
  bool deserialize(const uint8_t* data, size_t size, bool* newerVersion = nullptr);

  static std::string pathFor(const std::string& cachePath);
  // Loads <cache>/rhythm_days.bin (falling back to .bak); empty when missing.
  static BookDailyMinutes load(const std::string& cachePath);
  // Atomic write; refuses to replace a file written by a newer version.
  bool save(const std::string& cachePath) const;
  // Session-commit hook: load, record, save.
  static void recordSpanAndSave(const std::string& cachePath, const ReadingStatsDateTime& localStart, uint32_t seconds);
};
