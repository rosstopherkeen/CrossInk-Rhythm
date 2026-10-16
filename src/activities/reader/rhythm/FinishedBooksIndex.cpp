// Ported from XPoint (https://github.com/Belphemur/XPoint, MIT):
// src/activities/reader/FinishedBooksIndex.cpp. File format, entry limits and
// sort order are unchanged; keys are cache paths and writes go through the
// shared StatsSidecarFile writer (byte-verified temp file, .bak rotation).
#include "FinishedBooksIndex.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

#include "StatsSidecarFile.h"

#ifndef READING_RHYTHM_TEST
#include "BookStatsScan.h"
#endif

namespace {
constexpr char LOG_TAG[] = "FBIDX";
constexpr char INDEX_PATH[] = "/.crosspoint/finished_books.bin";
constexpr char INDEX_BACKUP_PATH[] = "/.crosspoint/finished_books.bin.bak";
constexpr uint8_t INDEX_VERSION = 3;
constexpr uint8_t START_DATE_INDEX_VERSION = 2;
constexpr uint8_t LEGACY_INDEX_VERSION = 1;
constexpr uint16_t MAX_TITLE_BYTES = 160;
constexpr uint16_t MAX_AUTHOR_BYTES = 120;
constexpr uint8_t MAGIC[] = {'C', 'P', 'F', 'B'};
constexpr size_t HEADER_SIZE = sizeof(MAGIC) + 1 + 1 + 2;
constexpr size_t MAX_ENTRY_SIZE = 8 + 4 + 4 + 4 + 2 + MAX_TITLE_BYTES + 2 + MAX_AUTHOR_BYTES;
constexpr size_t MAX_FILE_SIZE = HEADER_SIZE + FinishedBooksIndex::MAX_ENTRIES * MAX_ENTRY_SIZE;

// Latched when a file written by a newer format is seen: writes are refused
// so entries this build cannot decode are never clobbered.
bool s_forwardVersionSeen = false;
// The fallback scan walks every epub_* cache; run it at most once per boot.
bool s_scannedThisBoot = false;

// Bounded little-endian reader over an in-memory copy of the index.
struct Cursor {
  const uint8_t* data;
  size_t size;
  size_t pos = 0;

  bool bytes(void* out, const size_t n) {
    if (size - pos < n) return false;
    memcpy(out, data + pos, n);
    pos += n;
    return true;
  }
  bool u8(uint8_t& v) { return bytes(&v, 1); }
  bool u16(uint16_t& v) {
    uint8_t b[2];
    if (!bytes(b, 2)) return false;
    v = static_cast<uint16_t>(b[0] | (b[1] << 8));
    return true;
  }
  bool u32(uint32_t& v) {
    uint8_t b[4];
    if (!bytes(b, 4)) return false;
    v = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) | (static_cast<uint32_t>(b[2]) << 16) |
        (static_cast<uint32_t>(b[3]) << 24);
    return true;
  }
  bool u64(uint64_t& v) {
    uint32_t lo = 0;
    uint32_t hi = 0;
    if (!u32(lo) || !u32(hi)) return false;
    v = static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
    return true;
  }
  bool date(ReadingStatsDate& d) { return u16(d.year) && u8(d.month) && u8(d.day); }
  bool str(std::string& s, const uint16_t maxBytes) {
    uint16_t length = 0;
    if (!u16(length) || length > maxBytes || size - pos < length) return false;
    s.assign(reinterpret_cast<const char*>(data + pos), length);
    pos += length;
    return true;
  }
};

struct Writer {
  uint8_t* data;
  size_t pos = 0;

  void bytes(const void* in, const size_t n) {
    memcpy(data + pos, in, n);
    pos += n;
  }
  void u8(const uint8_t v) { data[pos++] = v; }
  void u16(const uint16_t v) {
    u8(static_cast<uint8_t>(v & 0xFF));
    u8(static_cast<uint8_t>(v >> 8));
  }
  void u32(const uint32_t v) {
    u16(static_cast<uint16_t>(v & 0xFFFF));
    u16(static_cast<uint16_t>(v >> 16));
  }
  void u64(const uint64_t v) {
    u32(static_cast<uint32_t>(v & 0xFFFFFFFFu));
    u32(static_cast<uint32_t>(v >> 32));
  }
  void date(const ReadingStatsDate& d) {
    u16(d.year);
    u8(d.month);
    u8(d.day);
  }
  void str(const std::string& s, const uint16_t maxBytes) {
    const uint16_t length = static_cast<uint16_t>(std::min<size_t>(s.size(), maxBytes));
    u16(length);
    bytes(s.data(), length);
  }
};

enum class LoadResult : uint8_t { Ok, Missing, Invalid, NewerFormat };

LoadResult decode(const uint8_t* data, const size_t size, std::vector<FinishedBookEntry>* entries) {
  Cursor in{data, size};
  uint8_t magic[sizeof(MAGIC)];
  uint8_t version = 0;
  uint8_t count = 0;
  uint16_t reserved = 0;
  if (!in.bytes(magic, sizeof(magic)) || memcmp(magic, MAGIC, sizeof(MAGIC)) != 0 || !in.u8(version) || !in.u8(count) ||
      !in.u16(reserved)) {
    return LoadResult::Invalid;
  }
  if (version > INDEX_VERSION) return LoadResult::NewerFormat;
  if (version < LEGACY_INDEX_VERSION || count > FinishedBooksIndex::MAX_ENTRIES) return LoadResult::Invalid;

  std::vector<FinishedBookEntry> decoded;
  decoded.reserve(count);
  for (uint8_t i = 0; i < count; ++i) {
    FinishedBookEntry entry;
    bool ok = in.u64(entry.pathKey) && in.u32(entry.totalReadingSeconds);
    if (ok && version >= START_DATE_INDEX_VERSION) ok = in.date(entry.startDate);
    ok = ok && in.date(entry.finishedDate) && in.str(entry.title, MAX_TITLE_BYTES);
    if (ok && version >= INDEX_VERSION) ok = in.str(entry.author, MAX_AUTHOR_BYTES);
    if (!ok) return LoadResult::Invalid;
    if (!entry.title.empty()) decoded.push_back(std::move(entry));
  }
  if (in.pos != size) return LoadResult::Invalid;
  if (entries) *entries = std::move(decoded);
  return LoadResult::Ok;
}

LoadResult loadPath(const char* path, std::vector<FinishedBookEntry>* entries) {
  if (!Storage.exists(path)) return LoadResult::Missing;
  HalFile file;
  if (!Storage.openFileForRead(LOG_TAG, path, file)) return LoadResult::Invalid;
  const size_t size = file.fileSize();
  if (size < HEADER_SIZE || size > MAX_FILE_SIZE) {
    // A longer file may still be a newer format; check its header for the guard.
    uint8_t header[HEADER_SIZE];
    const bool headerRead = size >= HEADER_SIZE && file.read(header, sizeof(header)) == static_cast<int>(HEADER_SIZE);
    file.close();
    if (headerRead && memcmp(header, MAGIC, sizeof(MAGIC)) == 0 && header[4] > INDEX_VERSION) {
      return LoadResult::NewerFormat;
    }
    return LoadResult::Invalid;
  }
  // At most ~9.7 KB, read once per screen open or finish event.
  auto buffer = makeUniqueNoThrow<uint8_t[]>(size);
  if (!buffer) {
    file.close();
    LOG_ERR(LOG_TAG, "Out of memory reading finished-books index (%u bytes)", static_cast<unsigned>(size));
    return LoadResult::Invalid;
  }
  const int n = file.read(buffer.get(), size);
  file.close();
  if (n != static_cast<int>(size)) return LoadResult::Invalid;
  return decode(buffer.get(), size, entries);
}

bool decodesAt(const char* path) { return loadPath(path, nullptr) == LoadResult::Ok; }

bool writeIndex(const std::vector<FinishedBookEntry>& entries) {
  if (s_forwardVersionSeen) {
    LOG_ERR(LOG_TAG, "Refusing to overwrite finished-books index from a newer firmware");
    return false;
  }
  const size_t count = std::min(entries.size(), FinishedBooksIndex::MAX_ENTRIES);
  size_t size = HEADER_SIZE;
  for (size_t i = 0; i < count; ++i) {
    size += 8 + 4 + 4 + 4 + 2 + std::min<size_t>(entries[i].title.size(), MAX_TITLE_BYTES) + 2 +
            std::min<size_t>(entries[i].author.size(), MAX_AUTHOR_BYTES);
  }
  auto buffer = makeUniqueNoThrow<uint8_t[]>(size);
  if (!buffer) {
    LOG_ERR(LOG_TAG, "Out of memory writing finished-books index (%u bytes)", static_cast<unsigned>(size));
    return false;
  }

  Writer out{buffer.get()};
  out.bytes(MAGIC, sizeof(MAGIC));
  out.u8(INDEX_VERSION);
  out.u8(static_cast<uint8_t>(count));
  out.u16(0);
  for (size_t i = 0; i < count; ++i) {
    const auto& entry = entries[i];
    out.u64(entry.pathKey);
    out.u32(entry.totalReadingSeconds);
    out.date(entry.startDate);
    out.date(entry.finishedDate);
    out.str(entry.title, MAX_TITLE_BYTES);
    out.str(entry.author, MAX_AUTHOR_BYTES);
  }
  return StatsSidecarFile::writeAtomically(LOG_TAG, INDEX_PATH, buffer.get(), out.pos, decodesAt);
}

bool sameBook(const FinishedBookEntry& a, const FinishedBookEntry& b) {
  return a.pathKey == b.pathKey || (a.title == b.title && a.author == b.author);
}

#ifdef READING_RHYTHM_TEST
std::vector<FinishedBookEntry> s_testScanResult;

std::vector<FinishedBookEntry> scanFinishedBooks() { return s_testScanResult; }
#else
void collectFinished(const BookStatsScan::Book& book, void* ctx) {
  if (!book.stats.isCompleted || book.title.empty()) return;
  auto& found = *static_cast<std::vector<FinishedBookEntry>*>(ctx);
  found.push_back({FinishedBooksIndex::keyForCachePath(book.cachePath), book.title, book.author,
                   book.stats.totalReadingSeconds, book.stats.startDate, book.stats.finishedDate});
}

std::vector<FinishedBookEntry> scanFinishedBooks() {
  std::vector<FinishedBookEntry> found;
  found.reserve(FinishedBooksIndex::MAX_ENTRIES);
  BookStatsScan::forEach(collectFinished, &found);
  return found;
}
#endif
}  // namespace

uint64_t FinishedBooksIndex::keyForCachePath(const std::string& cachePath) {
  uint64_t hash = 14695981039346656037ull;
  for (const unsigned char c : cachePath) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  return hash;
}

void FinishedBooksIndex::sortEntries(std::vector<FinishedBookEntry>& entries) {
  std::sort(entries.begin(), entries.end(), [](const FinishedBookEntry& a, const FinishedBookEntry& b) {
    if (a.finishedDate.isValid() != b.finishedDate.isValid()) return a.finishedDate.isValid();
    if (a.finishedDate.isValid()) {
      const int order = compareReadingStatsDate(a.finishedDate, b.finishedDate);
      if (order != 0) return order > 0;
    }
    return a.title < b.title;
  });
}

bool FinishedBooksIndex::mergeCandidates(std::vector<FinishedBookEntry>& entries,
                                         const std::vector<FinishedBookEntry>& found) {
  bool added = false;
  for (const auto& candidate : found) {
    if (candidate.title.empty()) continue;
    const bool present = std::any_of(entries.begin(), entries.end(),
                                     [&candidate](const FinishedBookEntry& e) { return sameBook(e, candidate); });
    if (present) continue;
    entries.push_back(candidate);
    added = true;
  }
  if (added) {
    sortEntries(entries);
    if (entries.size() > MAX_ENTRIES) entries.resize(MAX_ENTRIES);
  }
  return added;
}

std::vector<FinishedBookEntry> FinishedBooksIndex::load(const uint32_t completedBooksHint) {
  std::vector<FinishedBookEntry> entries;
  LoadResult primary = loadPath(INDEX_PATH, &entries);
  LoadResult result = primary;
  if (primary == LoadResult::NewerFormat) {
    s_forwardVersionSeen = true;
    LOG_ERR(LOG_TAG, "finished_books.bin is from a newer build; leaving it untouched");
    return {};
  }
  if (primary != LoadResult::Ok) {
    result = loadPath(INDEX_BACKUP_PATH, &entries);
    if (result == LoadResult::NewerFormat) {
      s_forwardVersionSeen = true;
      return {};
    }
    if (result == LoadResult::Ok) LOG_ERR(LOG_TAG, "Primary finished-books index invalid, using backup");
  }

  const bool noIndexYet = primary == LoadResult::Missing && result == LoadResult::Missing;
  const bool looksIncomplete = completedBooksHint > entries.size() && entries.size() < MAX_ENTRIES;
  if (!s_scannedThisBoot && (noIndexYet || looksIncomplete)) {
    s_scannedThisBoot = true;
    const bool added = mergeCandidates(entries, scanFinishedBooks());
    if ((added || noIndexYet) && !writeIndex(entries)) {
      LOG_ERR(LOG_TAG, "Could not persist finished-books scan");
    }
  }
  sortEntries(entries);
  return entries;
}

bool FinishedBooksIndex::record(const std::string& cachePath, const std::string& title, const std::string& author,
                                const BookReadingStats& stats) {
  if (cachePath.empty()) return false;
  auto entries = load();
  if (s_forwardVersionSeen) return false;
  const uint64_t key = keyForCachePath(cachePath);
  const auto it =
      std::find_if(entries.begin(), entries.end(), [key](const FinishedBookEntry& e) { return e.pathKey == key; });

  if (!stats.isCompleted) {
    if (it == entries.end()) return true;
    entries.erase(it);
    return writeIndex(entries);
  }
  if (title.empty()) {
    LOG_ERR(LOG_TAG, "Skipping finished book without title: %s", cachePath.c_str());
    return false;
  }

  const FinishedBookEntry updated{key, title, author, stats.totalReadingSeconds, stats.startDate, stats.finishedDate};
  if (it != entries.end()) {
    if (it->title == updated.title && it->author == updated.author &&
        it->totalReadingSeconds == updated.totalReadingSeconds &&
        compareReadingStatsDate(it->startDate, updated.startDate) == 0 &&
        compareReadingStatsDate(it->finishedDate, updated.finishedDate) == 0) {
      return true;
    }
    *it = updated;
  } else {
    // A scan may have found this book before; drop that copy by title/author.
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&updated](const FinishedBookEntry& e) { return sameBook(e, updated); }),
                  entries.end());
    entries.push_back(updated);
  }
  sortEntries(entries);
  if (entries.size() > MAX_ENTRIES) entries.resize(MAX_ENTRIES);
  return writeIndex(entries);
}

bool FinishedBooksIndex::migrateCachePath(const std::string& oldCachePath, const std::string& newCachePath) {
  if (oldCachePath.empty() || newCachePath.empty() || oldCachePath == newCachePath) return true;
  // Plain read: a move must not trigger the fallback scan.
  std::vector<FinishedBookEntry> entries;
  if (loadPath(INDEX_PATH, &entries) != LoadResult::Ok && loadPath(INDEX_BACKUP_PATH, &entries) != LoadResult::Ok) {
    return true;
  }
  const uint64_t oldKey = keyForCachePath(oldCachePath);
  const uint64_t newKey = keyForCachePath(newCachePath);
  const auto hasKey = [](const uint64_t key) { return [key](const FinishedBookEntry& e) { return e.pathKey == key; }; };
  if (std::none_of(entries.begin(), entries.end(), hasKey(oldKey))) return true;
  // Drop any stale entry already keyed to the destination, then re-key.
  entries.erase(std::remove_if(entries.begin(), entries.end(), hasKey(newKey)), entries.end());
  std::find_if(entries.begin(), entries.end(), hasKey(oldKey))->pathKey = newKey;
  return writeIndex(entries);
}

#ifdef READING_RHYTHM_TEST
void FinishedBooksIndex::setScanResultForTest(const std::vector<FinishedBookEntry>& found) { s_testScanResult = found; }

void FinishedBooksIndex::resetForTest() {
  s_testScanResult.clear();
  s_forwardVersionSeen = false;
  s_scannedThisBoot = false;
}
#endif
