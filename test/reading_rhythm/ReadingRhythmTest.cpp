// Host tests for the Reading Rhythm / Books Finished sidecar files and the
// one-time minutes backfill (screens ported from XPoint, MIT).
#include <HalClock.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <string>
#include <vector>

#include "activities/reader/ReadingStatsUtils.h"
#include "activities/reader/rhythm/BookDailyMinutes.h"
#include "activities/reader/rhythm/BookRhythm.h"
#include "activities/reader/rhythm/DailyMinutesBackfill.h"
#include "activities/reader/rhythm/DailyMinutesLog.h"
#include "activities/reader/rhythm/FinishedBooksIndex.h"
#include "activities/reader/rhythm/StatsSidecarFile.h"

namespace {

constexpr char MINUTES_PATH[] = "/.crosspoint/daily_minutes.bin";
constexpr char MINUTES_BAK[] = "/.crosspoint/daily_minutes.bin.bak";
constexpr char INDEX_PATH[] = "/.crosspoint/finished_books.bin";
constexpr char INDEX_BAK[] = "/.crosspoint/finished_books.bin.bak";
constexpr char GLOBAL_PATH[] = "/.crosspoint/global_stats.bin";

uint32_t day(const uint16_t y, const uint8_t m, const uint8_t d) { return readingStatsDayIndex({y, m, d}); }

ReadingStatsDateTime at(const uint16_t y, const uint8_t m, const uint8_t d, const uint8_t h, const uint8_t mi) {
  ReadingStatsDateTime dt;
  dt.date = {y, m, d};
  dt.hour = h;
  dt.minute = mi;
  return dt;
}

uint16_t le16(const std::vector<uint8_t>& b, const size_t o) { return static_cast<uint16_t>(b[o] | (b[o + 1] << 8)); }
uint32_t le32(const std::vector<uint8_t>& b, const size_t o) {
  return static_cast<uint32_t>(b[o]) | (static_cast<uint32_t>(b[o + 1]) << 8) |
         (static_cast<uint32_t>(b[o + 2]) << 16) | (static_cast<uint32_t>(b[o + 3]) << 24);
}

struct History {
  uint32_t anchor = 0;
  std::array<uint8_t, READING_HISTORY_BYTES> bits{};
  void mark(const uint32_t dayIndex) { markReadingHistoryDay(anchor, bits, dayIndex); }
};

class RhythmTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Storage.reset();
    DailyMinutesLog::resetForwardGuardForTest();
    FinishedBooksIndex::resetForTest();
    halClock.available = false;
  }
};

// ---------------------------------------------------------------- daily minutes

TEST_F(RhythmTest, WireLayoutMatchesDocumentedOffsets) {
  DailyMinutesLog log;
  log.flags = DailyMinutesLog::FLAG_BACKFILL_DONE;
  log.anchorDay = 0x01020304;
  log.firstRealDay = 0x0A0B0C0D;
  log.realMinutes[0] = 0x1234;
  log.realMinutes[90] = 7;
  log.estimatedMinutes[0] = 0x0456;
  log.estimatedMinutes[90] = 9;
  std::vector<uint8_t> b(DailyMinutesLog::FILE_SIZE);
  log.serialize(b.data());

  ASSERT_EQ(DailyMinutesLog::FILE_SIZE, 380u);
  EXPECT_EQ(std::string(b.begin(), b.begin() + 4), "CIDM");
  EXPECT_EQ(b[4], 1);
  EXPECT_EQ(b[5], DailyMinutesLog::FLAG_BACKFILL_DONE);
  EXPECT_EQ(le16(b, 6), 91);
  EXPECT_EQ(le32(b, 8), 0x01020304u);
  EXPECT_EQ(le32(b, 12), 0x0A0B0C0Du);
  EXPECT_EQ(le16(b, 16), 0x1234);
  EXPECT_EQ(le16(b, 16 + 90 * 2), 7);
  EXPECT_EQ(le16(b, 198), 0x0456);
  EXPECT_EQ(le16(b, 198 + 90 * 2), 9);
}

TEST_F(RhythmTest, SaveLoadRoundTrip) {
  DailyMinutesLog log;
  log.recordSpan(at(2026, 10, 16, 9, 0), 25 * 60);
  ASSERT_TRUE(log.save());
  ASSERT_EQ(Storage.bytes(MINUTES_PATH).size(), DailyMinutesLog::FILE_SIZE);

  const DailyMinutesLog loaded = DailyMinutesLog::load();
  EXPECT_EQ(loaded.anchorDay, day(2026, 10, 16));
  EXPECT_EQ(loaded.firstRealDay, day(2026, 10, 16));
  EXPECT_EQ(loaded.realMinutesOnDay(day(2026, 10, 16)), 25);
  EXPECT_FALSE(loaded.backfillDone());
}

TEST_F(RhythmTest, SpanSplitsAtMidnightAndRoundsPerSegment) {
  DailyMinutesLog log;
  log.recordSpan(at(2026, 10, 15, 23, 50), 20 * 60);
  EXPECT_EQ(log.realMinutesOnDay(day(2026, 10, 15)), 10);
  EXPECT_EQ(log.realMinutesOnDay(day(2026, 10, 16)), 10);
  EXPECT_EQ(log.anchorDay, day(2026, 10, 16));

  DailyMinutesLog rounding;
  rounding.recordSpan(at(2026, 10, 16, 8, 0), 29);
  EXPECT_EQ(rounding.realMinutesOnDay(day(2026, 10, 16)), 0);
  rounding.recordSpan(at(2026, 10, 16, 9, 0), 30);
  EXPECT_EQ(rounding.realMinutesOnDay(day(2026, 10, 16)), 1);
}

TEST_F(RhythmTest, WindowShiftDropsDaysOlderThan91) {
  DailyMinutesLog log;
  log.recordSpan(at(2026, 7, 1, 12, 0), 600);
  log.recordSpan(at(2026, 7, 3, 12, 0), 600);
  log.recordSpan(at(2026, 10, 1, 12, 0), 600);  // slot 92 for 07-01, slot 90 (last kept) for 07-03
  EXPECT_EQ(log.realMinutesOnDay(day(2026, 7, 1)), 0);
  EXPECT_EQ(log.realMinutesOnDay(day(2026, 7, 3)), 10);
  EXPECT_EQ(log.realMinutesOnDay(day(2026, 10, 1)), 10);

  log.recordSpan(at(2027, 6, 1, 12, 0), 60);
  EXPECT_EQ(log.realMinutesOnDay(day(2026, 10, 1)), 0);
  EXPECT_EQ(log.realMinutesOnDay(day(2027, 6, 1)), 1);
}

TEST_F(RhythmTest, MinutesClampAt1440PerDay) {
  DailyMinutesLog log;
  for (int i = 0; i < 30; ++i) log.recordSpan(at(2026, 10, 16, 0, 0), 23 * 3600);
  EXPECT_EQ(log.realMinutesOnDay(day(2026, 10, 16)), 1440);
  log.addEstimatedMinutes(day(2026, 10, 16), 5000);
  EXPECT_EQ(log.estimatedMinutesOnDay(day(2026, 10, 16)), 1440);
}

TEST_F(RhythmTest, RealMinutesOverrideEstimates) {
  DailyMinutesLog log;
  log.advanceTo(day(2026, 10, 16));
  log.addEstimatedMinutes(day(2026, 10, 15), 40);
  log.addEstimatedMinutes(day(2026, 10, 14), 25);
  log.recordSpan(at(2026, 10, 15, 20, 0), 5 * 60);

  bool estimated = true;
  EXPECT_EQ(log.minutesOnDay(day(2026, 10, 15), &estimated), 5);
  EXPECT_FALSE(estimated);
  EXPECT_EQ(log.minutesOnDay(day(2026, 10, 14), &estimated), 25);
  EXPECT_TRUE(estimated);
  EXPECT_EQ(log.minutesOnDay(day(2026, 10, 13), &estimated), 0);
  EXPECT_FALSE(estimated);
}

TEST_F(RhythmTest, CorruptPrimaryRecoversFromBackup) {
  DailyMinutesLog log;
  log.recordSpan(at(2026, 10, 16, 9, 0), 600);
  ASSERT_TRUE(log.save());
  log.recordSpan(at(2026, 10, 16, 10, 0), 600);
  ASSERT_TRUE(log.save());  // previous file rotated to .bak
  ASSERT_FALSE(Storage.bytes(MINUTES_BAK).empty());

  Storage.put(MINUTES_PATH, {1, 2, 3});
  EXPECT_EQ(DailyMinutesLog::load().realMinutesOnDay(day(2026, 10, 16)), 10);

  // Saving over a corrupt primary keeps the decodable backup.
  const std::vector<uint8_t> backup = Storage.bytes(MINUTES_BAK);
  ASSERT_TRUE(log.save());
  EXPECT_EQ(Storage.bytes(MINUTES_BAK), backup);
  EXPECT_EQ(DailyMinutesLog::load().realMinutesOnDay(day(2026, 10, 16)), 20);
}

TEST_F(RhythmTest, NewerVersionFileIsNeverOverwritten) {
  std::vector<uint8_t> future(DailyMinutesLog::FILE_SIZE + 10, 0);
  memcpy(future.data(), "CIDM", 4);
  future[4] = 2;
  Storage.put(MINUTES_PATH, future);

  const DailyMinutesLog loaded = DailyMinutesLog::load();
  EXPECT_TRUE(loaded.isEmpty());
  DailyMinutesLog::recordSpanAndSave(at(2026, 10, 16, 9, 0), 600);
  EXPECT_EQ(Storage.bytes(MINUTES_PATH), future);
}

TEST_F(RhythmTest, FailedInstallRestoresPrimaryFromBackup) {
  DailyMinutesLog log;
  log.recordSpan(at(2026, 10, 16, 9, 0), 600);
  ASSERT_TRUE(log.save());
  const std::vector<uint8_t> before = Storage.bytes(MINUTES_PATH);

  log.recordSpan(at(2026, 10, 16, 10, 0), 600);
  Storage.failNextRenameFrom(std::string(MINUTES_PATH) + ".tmp");
  EXPECT_FALSE(log.save());
  EXPECT_EQ(Storage.bytes(MINUTES_PATH), before);
  EXPECT_FALSE(Storage.exists((std::string(MINUTES_PATH) + ".tmp").c_str()));
}

// ---------------------------------------------------------------- backfill math

TEST_F(RhythmTest, MinutesPerDayRoundsAndHasFloorOfOne) {
  EXPECT_EQ(DailyMinutesBackfill::minutesPerDay(3000, 5), 10u);
  EXPECT_EQ(DailyMinutesBackfill::minutesPerDay(3000 + 149, 5), 10u);
  EXPECT_EQ(DailyMinutesBackfill::minutesPerDay(3000 + 150, 5), 11u);
  EXPECT_EQ(DailyMinutesBackfill::minutesPerDay(10, 4), 1u);
  EXPECT_EQ(DailyMinutesBackfill::minutesPerDay(100, 0), 0u);
}

TEST_F(RhythmTest, BookTimeSpreadsEvenlyOverMinOfSessionsAndReadDays) {
  const uint32_t today = day(2026, 10, 16);
  History history;
  for (uint32_t d = today - 20; d < today - 10; ++d) history.mark(d);  // 10 read days

  DailyMinutesLog log;
  log.advanceTo(today);
  DailyMinutesBackfillBook book;
  book.startDay = today - 25;
  book.endDay = today - 5;
  book.totalReadingSeconds = 50 * 60;
  book.sessionCount = 5;
  DailyMinutesBackfill::apply(history.anchor, history.bits, today, &book, 1, log);

  EXPECT_TRUE(log.backfillDone());
  // k = min(5 sessions, 10 days) = 5; picks read-day ordinals 0,2,4,6,8.
  for (uint32_t i = 0; i < 10; ++i) {
    const uint32_t d = today - 20 + i;
    EXPECT_EQ(log.estimatedMinutesOnDay(d), i % 2 == 0 ? 10 : 1) << "day offset " << i;
  }
  EXPECT_EQ(log.estimatedMinutesOnDay(today - 21), 0);  // not a read day
  EXPECT_EQ(log.estimatedMinutesOnDay(today - 9), 0);
}

TEST_F(RhythmTest, MoreSessionsThanDaysUsesEveryReadDay) {
  const uint32_t today = day(2026, 10, 16);
  History history;
  history.mark(today - 3);
  history.mark(today - 2);
  history.mark(today - 1);

  DailyMinutesLog log;
  log.advanceTo(today);
  DailyMinutesBackfillBook book{today - 3, today - 1, 90 * 60, 40};
  DailyMinutesBackfill::apply(history.anchor, history.bits, today, &book, 1, log);
  EXPECT_EQ(log.estimatedMinutesOnDay(today - 3), 30);
  EXPECT_EQ(log.estimatedMinutesOnDay(today - 2), 30);
  EXPECT_EQ(log.estimatedMinutesOnDay(today - 1), 30);
}

TEST_F(RhythmTest, OngoingBookRunsToTodayAndOverlapsSum) {
  const uint32_t today = day(2026, 10, 16);
  History history;
  history.mark(today - 2);
  history.mark(today);

  DailyMinutesLog log;
  log.advanceTo(today);
  const DailyMinutesBackfillBook books[] = {
      {today - 10, today - 2, 20 * 60, 1},  // finished book, one session
      {today - 2, today, 60 * 60, 2},       // ongoing: start -> today
  };
  DailyMinutesBackfill::apply(history.anchor, history.bits, today, books, 2, log);
  EXPECT_EQ(log.estimatedMinutesOnDay(today - 2), 20 + 30);
  EXPECT_EQ(log.estimatedMinutesOnDay(today), 30);
}

TEST_F(RhythmTest, UncoveredReadDaysGetOneMinute) {
  const uint32_t today = day(2026, 10, 16);
  History history;
  history.mark(today - 40);
  history.mark(today - 7);

  DailyMinutesLog log;
  log.advanceTo(today);
  DailyMinutesBackfill::apply(history.anchor, history.bits, today, nullptr, 0, log);
  EXPECT_EQ(log.estimatedMinutesOnDay(today - 40), 1);
  EXPECT_EQ(log.estimatedMinutesOnDay(today - 7), 1);
  EXPECT_EQ(log.estimatedMinutesOnDay(today - 8), 0);
}

TEST_F(RhythmTest, NothingEstimatedAfterFirstRealDay) {
  const uint32_t today = day(2026, 10, 16);
  History history;
  for (uint32_t d = today - 4; d <= today; ++d) history.mark(d);

  DailyMinutesLog log;
  log.recordSpan(at(2026, 10, 14, 9, 0), 15 * 60);  // first real recording, today-2
  log.advanceTo(today);
  DailyMinutesBackfillBook book{today - 4, today, 100 * 60, 10};
  DailyMinutesBackfill::apply(history.anchor, history.bits, log.firstRealDay, &book, 1, log);

  // Days up to the cutoff (today-2) share the time; later days stay real-only.
  EXPECT_EQ(log.estimatedMinutesOnDay(today - 4), 33);
  EXPECT_EQ(log.estimatedMinutesOnDay(today - 3), 33);
  EXPECT_EQ(log.estimatedMinutesOnDay(today - 2), 33);
  EXPECT_EQ(log.estimatedMinutesOnDay(today - 1), 0);
  EXPECT_EQ(log.estimatedMinutesOnDay(today), 0);
  bool estimated = true;
  EXPECT_EQ(log.minutesOnDay(today - 2, &estimated), 15);  // real wins
  EXPECT_FALSE(estimated);
}

TEST_F(RhythmTest, BooksWithoutStartDateOnlyGetMarkers) {
  const uint32_t today = day(2026, 10, 16);
  History history;
  history.mark(today - 1);
  DailyMinutesLog log;
  log.advanceTo(today);
  DailyMinutesBackfillBook book{0, today, 3600, 3};
  DailyMinutesBackfill::apply(history.anchor, history.bits, today, &book, 1, log);
  EXPECT_EQ(log.estimatedMinutesOnDay(today - 1), 1);
}

// ---------------------------------------------------------------- finished books

BookReadingStats completedStats(const uint32_t seconds, const ReadingStatsDate start, const ReadingStatsDate finish) {
  BookReadingStats stats;
  stats.isCompleted = true;
  stats.totalReadingSeconds = seconds;
  stats.startDate = start;
  stats.finishedDate = finish;
  return stats;
}

FinishedBookEntry entry(const std::string& cache, const std::string& title, const ReadingStatsDate finish) {
  FinishedBookEntry e;
  e.pathKey = FinishedBooksIndex::keyForCachePath(cache);
  e.title = title;
  e.author = "Author";
  e.totalReadingSeconds = 3600;
  e.startDate = {2026, 1, 1};
  e.finishedDate = finish;
  return e;
}

TEST_F(RhythmTest, FirstLoadSeedsFromScanAndPersists) {
  FinishedBooksIndex::setScanResultForTest(
      {entry("/.crosspoint/epub_1", "Older", {2026, 3, 1}), entry("/.crosspoint/epub_2", "Newer", {2026, 9, 1})});
  const auto books = FinishedBooksIndex::load();
  ASSERT_EQ(books.size(), 2u);
  EXPECT_EQ(books[0].title, "Newer");
  EXPECT_EQ(books[1].title, "Older");
  EXPECT_FALSE(Storage.bytes(INDEX_PATH).empty());

  FinishedBooksIndex::resetForTest();  // new boot, empty scan result
  EXPECT_EQ(FinishedBooksIndex::load().size(), 2u);
}

TEST_F(RhythmTest, RecordRoundTripUpdateAndRemove) {
  const std::string cache = "/.crosspoint/epub_abc";
  ASSERT_TRUE(FinishedBooksIndex::record(cache, "Dune", "Herbert", completedStats(7200, {2026, 9, 1}, {2026, 10, 2})));
  auto books = FinishedBooksIndex::load();
  ASSERT_EQ(books.size(), 1u);
  EXPECT_EQ(books[0].title, "Dune");
  EXPECT_EQ(books[0].author, "Herbert");
  EXPECT_EQ(books[0].totalReadingSeconds, 7200u);
  EXPECT_EQ(compareReadingStatsDate(books[0].startDate, {2026, 9, 1}), 0);
  EXPECT_EQ(compareReadingStatsDate(books[0].finishedDate, {2026, 10, 2}), 0);

  ASSERT_TRUE(FinishedBooksIndex::record(cache, "Dune", "Herbert", completedStats(9000, {2026, 9, 1}, {2026, 10, 3})));
  books = FinishedBooksIndex::load();
  ASSERT_EQ(books.size(), 1u);
  EXPECT_EQ(books[0].totalReadingSeconds, 9000u);

  BookReadingStats unfinished;
  ASSERT_TRUE(FinishedBooksIndex::record(cache, "Dune", "Herbert", unfinished));
  EXPECT_TRUE(FinishedBooksIndex::load().empty());
}

TEST_F(RhythmTest, CapsAtMaxEntriesKeepingNewest) {
  for (int i = 0; i < 40; ++i) {
    const ReadingStatsDate finish{2025, static_cast<uint8_t>(1 + i % 12), static_cast<uint8_t>(1 + i / 12)};
    ASSERT_TRUE(FinishedBooksIndex::record("/.crosspoint/epub_" + std::to_string(i), "Book " + std::to_string(i), "A",
                                           completedStats(60, {2025, 1, 1}, finish)));
  }
  const auto books = FinishedBooksIndex::load();
  EXPECT_EQ(books.size(), FinishedBooksIndex::MAX_ENTRIES);
  for (size_t i = 1; i < books.size(); ++i) {
    EXPECT_GE(compareReadingStatsDate(books[i - 1].finishedDate, books[i].finishedDate), 0);
  }
}

TEST_F(RhythmTest, RescanMergesMissingBooksWithoutDuplicates) {
  ASSERT_TRUE(FinishedBooksIndex::record("/.crosspoint/epub_a", "Known", "Author",
                                         completedStats(60, {2026, 1, 1}, {2026, 2, 1})));
  FinishedBooksIndex::resetForTest();
  FinishedBooksIndex::setScanResultForTest({
      entry("/.crosspoint/epub_a", "Known", {2026, 2, 1}),      // same key
      entry("/.crosspoint/epub_other", "Known", {2026, 2, 1}),  // same title/author
      entry("/.crosspoint/epub_lost", "Fell out of recents", {2026, 5, 5}),
  });
  // Global stats say 3 books were finished, the index only has 1: rescan.
  const auto books = FinishedBooksIndex::load(3);
  ASSERT_EQ(books.size(), 2u);
  EXPECT_EQ(books[0].title, "Fell out of recents");
  EXPECT_EQ(books[1].title, "Known");
}

TEST_F(RhythmTest, MigrateCachePathFollowsMovedBook) {
  ASSERT_TRUE(FinishedBooksIndex::record("/.crosspoint/epub_old", "Moved", "A",
                                         completedStats(60, {2026, 1, 1}, {2026, 2, 1})));
  ASSERT_TRUE(FinishedBooksIndex::migrateCachePath("/.crosspoint/epub_old", "/.crosspoint/epub_new"));
  const auto books = FinishedBooksIndex::load();
  ASSERT_EQ(books.size(), 1u);
  EXPECT_EQ(books[0].pathKey, FinishedBooksIndex::keyForCachePath("/.crosspoint/epub_new"));

  // Re-recording under the new path updates instead of duplicating.
  ASSERT_TRUE(FinishedBooksIndex::record("/.crosspoint/epub_new", "Moved", "A",
                                         completedStats(120, {2026, 1, 1}, {2026, 2, 1})));
  EXPECT_EQ(FinishedBooksIndex::load().size(), 1u);
}

TEST_F(RhythmTest, ReadsXPointV3IndexBytes) {
  // Hand-built XPoint-format v3 file with one entry.
  std::vector<uint8_t> b = {'C', 'P', 'F', 'B', 3, 1, 0, 0};
  const uint64_t key = 0x1122334455667788ull;
  for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>(key >> (8 * i)));
  for (const uint8_t v : {0x10, 0x0E, 0x00, 0x00}) b.push_back(v);  // 3600 s
  for (const uint8_t v : {0xEA, 0x07, 9, 1}) b.push_back(v);        // 2026-09-01
  for (const uint8_t v : {0xEA, 0x07, 10, 2}) b.push_back(v);       // 2026-10-02
  b.push_back(2);
  b.push_back(0);
  b.push_back('H');
  b.push_back('i');
  b.push_back(1);
  b.push_back(0);
  b.push_back('X');
  Storage.put(INDEX_PATH, b);

  const auto books = FinishedBooksIndex::load();
  ASSERT_EQ(books.size(), 1u);
  EXPECT_EQ(books[0].pathKey, key);
  EXPECT_EQ(books[0].title, "Hi");
  EXPECT_EQ(books[0].author, "X");
  EXPECT_EQ(books[0].totalReadingSeconds, 3600u);
  EXPECT_EQ(compareReadingStatsDate(books[0].finishedDate, {2026, 10, 2}), 0);

  // Our writer produces the same bytes for the same entry.
  FinishedBooksIndex::resetForTest();
  Storage.reset();
  FinishedBookEntry same = books[0];
  FinishedBooksIndex::setScanResultForTest({same});
  FinishedBooksIndex::load();
  EXPECT_EQ(Storage.bytes(INDEX_PATH), b);
}

TEST_F(RhythmTest, IndexBackupRecoveryAndForwardGuard) {
  ASSERT_TRUE(FinishedBooksIndex::record("/.crosspoint/epub_a", "A", "", completedStats(60, {}, {2026, 1, 1})));
  ASSERT_TRUE(FinishedBooksIndex::record("/.crosspoint/epub_b", "B", "", completedStats(60, {}, {2026, 1, 2})));
  Storage.put(INDEX_PATH, {'C', 'P', 'F'});
  EXPECT_EQ(FinishedBooksIndex::load().size(), 1u);  // backup holds the one-entry version

  std::vector<uint8_t> future = {'C', 'P', 'F', 'B', 9, 0, 0, 0};
  Storage.put(INDEX_PATH, future);
  FinishedBooksIndex::resetForTest();
  EXPECT_TRUE(FinishedBooksIndex::load().empty());
  EXPECT_FALSE(FinishedBooksIndex::record("/.crosspoint/epub_c", "C", "", completedStats(60, {}, {2026, 1, 3})));
  EXPECT_EQ(Storage.bytes(INDEX_PATH), future);
  (void)INDEX_BAK;
}

// ---------------------------------------------------------------- existing data

TEST_F(RhythmTest, SidecarsNeverTouchGlobalStats) {
  std::vector<uint8_t> global(159, 0);
  global[0] = 3;
  for (size_t i = 1; i < global.size(); ++i) global[i] = static_cast<uint8_t>(i * 7);
  Storage.put(GLOBAL_PATH, global);

  DailyMinutesLog::recordSpanAndSave(at(2026, 10, 16, 9, 0), 1200);
  FinishedBooksIndex::record("/.crosspoint/epub_a", "A", "", completedStats(60, {}, {2026, 1, 1}));
  FinishedBooksIndex::migrateCachePath("/.crosspoint/epub_a", "/.crosspoint/epub_b");

  EXPECT_EQ(Storage.bytes(GLOBAL_PATH), global);
  EXPECT_FALSE(Storage.exists("/.crosspoint/global_stats.bin.bak"));
  EXPECT_FALSE(Storage.exists("/.crosspoint/global_stats.bin.tmp"));
}

// ---------------------------------------------------------------- per-book minutes

TEST_F(RhythmTest, BookMinutesWireLayoutAndRoundTrip) {
  const std::string cache = "/.crosspoint/epub_7";
  BookDailyMinutes book;
  book.recordSpan(at(2026, 10, 15, 23, 50), 20 * 60);  // 10 + 10 across midnight
  ASSERT_EQ(book.days.size(), 2u);
  ASSERT_TRUE(book.save(cache));

  const std::vector<uint8_t> b = Storage.bytes(cache + "/rhythm_days.bin");
  ASSERT_EQ(b.size(), 8u + 2u * 6u);
  EXPECT_EQ(std::string(b.begin(), b.begin() + 4), "CIBR");
  EXPECT_EQ(b[4], 1);
  EXPECT_EQ(le16(b, 6), 2);
  EXPECT_EQ(le32(b, 8), day(2026, 10, 15));
  EXPECT_EQ(le16(b, 12), 10);
  EXPECT_EQ(le32(b, 14), day(2026, 10, 16));
  EXPECT_EQ(le16(b, 18), 10);

  const BookDailyMinutes loaded = BookDailyMinutes::load(cache);
  EXPECT_EQ(loaded.minutesOn(day(2026, 10, 15)), 10);
  EXPECT_EQ(loaded.minutesOn(day(2026, 10, 16)), 10);
  EXPECT_EQ(loaded.minutesOn(day(2026, 10, 17)), 0);
}

TEST_F(RhythmTest, BookMinutesCapDropsOldestAndClamps) {
  BookDailyMinutes book;
  for (uint32_t i = 0; i < BookDailyMinutes::MAX_DAYS + 5; ++i) {
    ReadingStatsDate date{2025, 1, 1};
    addDaysToReadingStatsDate(date, static_cast<int>(i));
    ReadingStatsDateTime dt;
    dt.date = date;
    dt.hour = 9;
    book.recordSpan(dt, 120);
  }
  EXPECT_EQ(book.days.size(), BookDailyMinutes::MAX_DAYS);
  EXPECT_EQ(book.minutesOn(day(2025, 1, 1)), 0);  // oldest dropped
  for (int i = 0; i < 30; ++i) book.recordSpan(at(2026, 10, 16, 0, 0), 23 * 3600);
  EXPECT_EQ(book.minutesOn(day(2026, 10, 16)), 1440);
}

TEST_F(RhythmTest, BookMinutesBackupAndForwardGuard) {
  const std::string cache = "/.crosspoint/epub_8";
  const std::string path = cache + "/rhythm_days.bin";
  BookDailyMinutes::recordSpanAndSave(cache, at(2026, 10, 16, 9, 0), 600);
  BookDailyMinutes::recordSpanAndSave(cache, at(2026, 10, 16, 10, 0), 600);
  Storage.put(path, {'C', 'I', 'B'});
  EXPECT_EQ(BookDailyMinutes::load(cache).minutesOn(day(2026, 10, 16)), 10);  // from .bak

  std::vector<uint8_t> future = {'C', 'I', 'B', 'R', 2, 0, 0, 0};
  Storage.put(path, future);
  BookDailyMinutes::recordSpanAndSave(cache, at(2026, 10, 17, 9, 0), 600);
  EXPECT_EQ(Storage.bytes(path), future);
}

// ---------------------------------------------------------------- per-book rhythm

BookReadingStats bookStats(const ReadingStatsDate start, const ReadingStatsDate finish, const uint32_t seconds,
                           const uint16_t sessions, const bool completed = true) {
  BookReadingStats stats;
  stats.startDate = start;
  stats.finishedDate = finish;
  stats.totalReadingSeconds = seconds;
  stats.sessionCount = sessions;
  stats.isCompleted = completed;
  return stats;
}

TEST_F(RhythmTest, FinishedBookEstimatesOnlyGlobalReadDaysInWindow) {
  History history;
  // Read days: Oct 1, 3, 5, 7 plus Sep 20 (before the book) and Oct 12 (after).
  for (const uint32_t d :
       {day(2026, 9, 20), day(2026, 10, 1), day(2026, 10, 3), day(2026, 10, 5), day(2026, 10, 7), day(2026, 10, 12)}) {
    history.mark(d);
  }
  BookRhythm rhythm;
  ASSERT_TRUE(BookRhythm::build(bookStats({2026, 10, 1}, {2026, 10, 7}, 40 * 60, 2), day(2026, 10, 16), history.anchor,
                                history.bits, BookDailyMinutes{}, rhythm));
  // k = min(2 sessions, 4 read days) = 2, evenly spaced: Oct 1 and Oct 5, 20 min each.
  EXPECT_EQ(rhythm.daysRead, 2);
  EXPECT_EQ(rhythm.totalMinutes, 40u);
  EXPECT_EQ(rhythm.dayAt(day(2026, 10, 1)).minutes, 20);
  EXPECT_TRUE(rhythm.dayAt(day(2026, 10, 1)).estimated);
  EXPECT_EQ(rhythm.dayAt(day(2026, 10, 5)).minutes, 20);
  EXPECT_EQ(rhythm.dayAt(day(2026, 10, 3)).minutes, 0);
  EXPECT_EQ(rhythm.dayAt(day(2026, 9, 20)).minutes, 0);
  EXPECT_EQ(rhythm.dayAt(day(2026, 10, 12)).minutes, 0);
  EXPECT_EQ(rhythm.spanDays, 7);
  EXPECT_EQ(rhythm.weekCount, 2);  // Mon Sep 28 and Mon Oct 5 weeks
  EXPECT_FALSE(rhythm.truncated);
  EXPECT_TRUE(rhythm.anyEstimated);
}

TEST_F(RhythmTest, OngoingBookWindowEndsTodayAndRealOverridesEstimates) {
  History history;
  for (const uint32_t d : {day(2026, 10, 10), day(2026, 10, 11), day(2026, 10, 14), day(2026, 10, 15)}) {
    history.mark(d);
  }
  BookDailyMinutes real;
  real.recordSpan(at(2026, 10, 14, 20, 0), 30 * 60);
  real.recordSpan(at(2026, 10, 15, 20, 0), 15 * 60);
  real.days.insert(real.days.begin(), BookDailyMinutes::Day{day(2026, 9, 1), 99});  // outside the window

  BookRhythm rhythm;
  ASSERT_TRUE(BookRhythm::build(bookStats({2026, 10, 10}, {}, (30 + 15 + 60) * 60, 6, false), day(2026, 10, 16),
                                history.anchor, history.bits, real, rhythm));
  EXPECT_EQ(rhythm.endDay, day(2026, 10, 16));
  // Real days use real minutes.
  EXPECT_EQ(rhythm.dayAt(day(2026, 10, 14)).minutes, 30);
  EXPECT_FALSE(rhythm.dayAt(day(2026, 10, 14)).estimated);
  EXPECT_EQ(rhythm.dayAt(day(2026, 10, 15)).minutes, 15);
  // Remaining 60 min spread over the 2 read days before the first real day.
  EXPECT_EQ(rhythm.dayAt(day(2026, 10, 10)).minutes, 30);
  EXPECT_TRUE(rhythm.dayAt(day(2026, 10, 10)).estimated);
  EXPECT_EQ(rhythm.dayAt(day(2026, 10, 11)).minutes, 30);
  EXPECT_EQ(rhythm.dayAt(day(2026, 9, 1)).minutes, 0);
  EXPECT_EQ(rhythm.totalMinutes, 105u);
  EXPECT_EQ(rhythm.daysRead, 4);
  EXPECT_EQ(rhythm.longestStreak, 2);
}

TEST_F(RhythmTest, LongBookShowsLastThirteenWeeks) {
  History history;
  for (uint32_t d = day(2026, 3, 1); d <= day(2026, 10, 1); d += 3) history.mark(d);
  BookRhythm rhythm;
  ASSERT_TRUE(BookRhythm::build(bookStats({2026, 3, 1}, {2026, 10, 1}, 3600 * 40, 80), day(2026, 10, 16),
                                history.anchor, history.bits, BookDailyMinutes{}, rhythm));
  EXPECT_TRUE(rhythm.truncated);
  EXPECT_EQ(rhythm.weekCount, BookRhythm::MAX_WEEKS);
  EXPECT_GT(rhythm.daysRead, 40);                       // summary covers the whole window, not just the shown weeks
  EXPECT_EQ(rhythm.dayAt(day(2026, 3, 1)).minutes, 0);  // before the shown weeks
}

TEST_F(RhythmTest, BookWithoutStartDateHasNoRhythm) {
  BookRhythm rhythm;
  EXPECT_FALSE(
      BookRhythm::build(bookStats({}, {2026, 10, 1}, 3600, 2), day(2026, 10, 16), 0, {}, BookDailyMinutes{}, rhythm));
  EXPECT_FALSE(rhythm.valid());
}

}  // namespace
