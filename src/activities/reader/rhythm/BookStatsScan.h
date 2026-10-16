#pragma once

#include <string>

#include "activities/reader/BookReadingStats.h"

// Read-only discovery of per-book stats for the Books Finished index and the
// Reading Rhythm minutes backfill.
//
// Sources, in order:
//   1. RECENT_BOOKS (title/author known, cache path derived from the book path)
//   2. every /.crosspoint/epub_* cache directory not already seen, so finished
//      books that dropped out of the 18-item recents list are still found.
//      Title/author come from the cache's book.bin header and are only read
//      for completed books.
//
// Only caches whose stats_v5.bin already exists are visited, so
// BookReadingStats::load() never takes its interrupted-save recovery path and
// the scan never renames or rewrites anything under /.crosspoint/.
namespace BookStatsScan {

struct Book {
  std::string cachePath;
  std::string title;
  std::string author;
  BookReadingStats stats;
};

using Visitor = void (*)(const Book& book, void* ctx);

void forEach(Visitor visit, void* ctx);

// Cache directory for an EPUB or XTC book path; empty for other formats.
std::string cachePathForBookPath(const std::string& bookPath);

}  // namespace BookStatsScan
