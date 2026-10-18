#include "BookStatsScan.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Xtc.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "FinishedBooksIndex.h"
#include "RecentBooksStore.h"

namespace {
constexpr char LOG_TAG[] = "BSCAN";
constexpr char CACHE_ROOT[] = "/.crosspoint";
constexpr char EPUB_CACHE_PREFIX[] = "epub_";
constexpr char STATS_FILE_NAME[] = "/stats_v5.bin";
constexpr char BOOK_BIN_NAME[] = "/book.bin";
constexpr uint32_t BOOK_CACHE_MAGIC = 0x425843FF;  // Mirrors BookMetadataCache.cpp
constexpr uint32_t MAX_METADATA_STRING = 512;

bool hasStatsFile(const std::string& cachePath) {
  return !cachePath.empty() && Storage.exists((cachePath + STATS_FILE_NAME).c_str());
}

bool readLe32(HalFile& file, uint32_t& value) {
  uint8_t bytes[4];
  if (file.read(bytes, sizeof(bytes)) != static_cast<int>(sizeof(bytes))) return false;
  value = static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
          (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
  return true;
}

bool readMetadataString(HalFile& file, std::string& out) {
  uint32_t length = 0;
  if (!readLe32(file, length) || length > MAX_METADATA_STRING) return false;
  out.resize(length);
  return length == 0 || file.read(&out[0], length) == static_cast<int>(length);
}

// Reads title and author from book.bin header A without going through
// BookMetadataCache::load(), which deletes caches it cannot parse.
// Layout: u32 magic, u8 version, u32 lutOffset, u16 spineCount, u16 tocCount,
// then length-prefixed title and author strings.
bool readEpubCacheTitle(const std::string& cachePath, std::string& title, std::string& author) {
  const std::string path = cachePath + BOOK_BIN_NAME;
  if (!Storage.exists(path.c_str())) return false;
  HalFile file;
  if (!Storage.openFileForRead(LOG_TAG, path, file)) return false;
  uint32_t magic = 0;
  uint8_t skipped[1 + 4 + 2 + 2];
  const bool ok = readLe32(file, magic) && magic == BOOK_CACHE_MAGIC &&
                  file.read(skipped, sizeof(skipped)) == static_cast<int>(sizeof(skipped)) &&
                  readMetadataString(file, title) && readMetadataString(file, author);
  file.close();
  if (!ok) {
    title.clear();
    author.clear();
  }
  return ok && !title.empty();
}
}  // namespace

namespace BookStatsScan {

std::string cachePathForBookPath(const std::string& bookPath) {
  if (bookPath.empty()) return {};
  if (FsHelpers::hasEpubExtension(bookPath)) return Epub::cachePathForFilePath(bookPath, CACHE_ROOT);
  if (FsHelpers::hasXtcExtension(bookPath)) return Xtc(bookPath, CACHE_ROOT).getCachePath();
  return {};
}

bool findCachePathForKey(const uint64_t key, std::string& outCachePath) {
  HalFile dir = Storage.open(CACHE_ROOT);
  if (!dir) return false;
  if (!dir.isDirectory()) {
    dir.close();
    return false;
  }
  char name[96];
  bool found = false;
  for (HalFile entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    const bool isDirectory = entry.isDirectory();
    const size_t nameLen = entry.getName(name, sizeof(name));
    entry.close();
    if (!isDirectory || nameLen == 0) continue;
    const std::string candidate = std::string(CACHE_ROOT) + "/" + name;
    if (FinishedBooksIndex::keyForCachePath(candidate) == key) {
      outCachePath = candidate;
      found = true;
      break;
    }
  }
  dir.close();
  if (!found) LOG_ERR(LOG_TAG, "No cache directory for finished book key %llx", static_cast<unsigned long long>(key));
  return found;
}

void forEach(const Visitor visit, void* ctx) {
  // Cache paths already visited through recents (at most MAX_RECENT_BOOKS).
  std::vector<std::string> seen;
  const auto& recentBooks = RECENT_BOOKS.getBooks();
  seen.reserve(recentBooks.size());

  for (const auto& recent : recentBooks) {
    Book book;
    book.cachePath = cachePathForBookPath(recent.path);
    if (!hasStatsFile(book.cachePath)) continue;
    seen.push_back(book.cachePath);
    book.title = recent.title;
    book.author = recent.author;
    book.stats = BookReadingStats::load(book.cachePath);
    visit(book, ctx);
  }

  HalFile dir = Storage.open(CACHE_ROOT);
  if (!dir) return;
  if (!dir.isDirectory()) {
    dir.close();
    return;
  }

  char name[96];
  uint16_t visited = 0;
  for (HalFile entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    const bool isDirectory = entry.isDirectory();
    const size_t nameLen = entry.getName(name, sizeof(name));
    entry.close();
    if (!isDirectory || nameLen == 0 || strncmp(name, EPUB_CACHE_PREFIX, sizeof(EPUB_CACHE_PREFIX) - 1) != 0) {
      continue;
    }

    Book book;
    book.cachePath = std::string(CACHE_ROOT) + "/" + name;
    if (std::find(seen.begin(), seen.end(), book.cachePath) != seen.end()) continue;
    if (!hasStatsFile(book.cachePath)) continue;
    book.stats = BookReadingStats::load(book.cachePath);
    if (book.stats.isCompleted) readEpubCacheTitle(book.cachePath, book.title, book.author);
    visit(book, ctx);
    ++visited;
  }
  dir.close();
  LOG_DBG(LOG_TAG, "Scanned %u recent and %u cached book stats", static_cast<unsigned>(seen.size()),
          static_cast<unsigned>(visited));
}

}  // namespace BookStatsScan
