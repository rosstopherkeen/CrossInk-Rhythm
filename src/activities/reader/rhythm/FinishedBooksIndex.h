#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "activities/reader/BookReadingStats.h"

// Ported from XPoint (https://github.com/Belphemur/XPoint, MIT):
// src/activities/reader/FinishedBooksIndex.{h,cpp}.
//
// Sidecar index of finished books for the Books Finished screen, persisted to
// /.crosspoint/finished_books.bin. The binary layout is XPoint's v3 format
// (magic "CPFB", u8 version, u8 count, u16 reserved, then per entry: u64 key,
// u32 seconds, start date, finish date, u16-prefixed title and author).
//
// CrossInk difference: the u64 key is FNV-1a over the book's cache directory
// (e.g. /.crosspoint/epub_<hash>) rather than the book path. Every source
// knows the cache path, including the fallback scan of epub_* caches where the
// original book path is unknown, so entries never duplicate.
struct FinishedBookEntry {
  uint64_t pathKey = 0;
  std::string title;
  std::string author;
  uint32_t totalReadingSeconds = 0;
  ReadingStatsDate startDate;
  ReadingStatsDate finishedDate;
};

class FinishedBooksIndex {
 public:
  static constexpr size_t MAX_ENTRIES = 32;

  static uint64_t keyForCachePath(const std::string& cachePath);

  // Loads the index (falling back to .bak). When no index exists yet, or when
  // completedBooksHint (GlobalReadingStats::completedBooks) says more books
  // were finished than the index holds, it scans recent books and the
  // /.crosspoint/epub_* caches once per boot and merges what it finds.
  static std::vector<FinishedBookEntry> load(uint32_t completedBooksHint = 0);

  // Adds, refreshes or (when stats.isCompleted is false) removes a book.
  static bool record(const std::string& cachePath, const std::string& title, const std::string& author,
                     const BookReadingStats& stats);

  // Re-keys an entry after its cache directory moved with the book.
  static bool migrateCachePath(const std::string& oldCachePath, const std::string& newCachePath);

  // Sorts newest finish date first, ties by title (XPoint order).
  static void sortEntries(std::vector<FinishedBookEntry>& entries);
  // Adds candidates whose key and title/author are not already present.
  // Returns true when anything was added.
  static bool mergeCandidates(std::vector<FinishedBookEntry>& entries, const std::vector<FinishedBookEntry>& found);

#ifdef READING_RHYTHM_TEST
  static void setScanResultForTest(const std::vector<FinishedBookEntry>& found);
  static void resetForTest();
#endif
};
