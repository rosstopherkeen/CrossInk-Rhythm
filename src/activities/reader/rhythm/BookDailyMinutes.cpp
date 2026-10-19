#include "BookDailyMinutes.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

#include "StatsSidecarFile.h"

namespace {
constexpr char LOG_TAG[] = "BDMIN";
constexpr char FILE_NAME[] = "/rhythm_days.bin";
constexpr uint8_t MAGIC[4] = {'C', 'I', 'B', 'R'};
constexpr size_t MAX_FILE_SIZE =
    BookDailyMinutes::HEADER_SIZE + BookDailyMinutes::MAX_DAYS * BookDailyMinutes::ENTRY_SIZE;

uint16_t readLe16(const uint8_t* d) { return static_cast<uint16_t>(d[0] | (d[1] << 8)); }
uint32_t readLe32(const uint8_t* d) {
  return static_cast<uint32_t>(d[0]) | (static_cast<uint32_t>(d[1]) << 8) | (static_cast<uint32_t>(d[2]) << 16) |
         (static_cast<uint32_t>(d[3]) << 24);
}
void writeLe16(uint8_t* d, const uint16_t v) {
  d[0] = static_cast<uint8_t>(v & 0xFF);
  d[1] = static_cast<uint8_t>(v >> 8);
}
void writeLe32(uint8_t* d, const uint32_t v) {
  writeLe16(d, static_cast<uint16_t>(v & 0xFFFF));
  writeLe16(d + 2, static_cast<uint16_t>(v >> 16));
}

enum class ReadResult : uint8_t { Ok, Missing, Invalid, NewerFormat };

ReadResult readFile(const std::string& path, BookDailyMinutes* out) {
  if (!Storage.exists(path.c_str())) return ReadResult::Missing;
  HalFile file;
  if (!Storage.openFileForRead(LOG_TAG, path, file)) return ReadResult::Invalid;
  const size_t size = file.fileSize();
  uint8_t header[BookDailyMinutes::HEADER_SIZE];
  if (size < sizeof(header) || file.read(header, sizeof(header)) != static_cast<int>(sizeof(header))) {
    file.close();
    return ReadResult::Invalid;
  }
  if (memcmp(header, MAGIC, sizeof(MAGIC)) != 0) {
    file.close();
    return ReadResult::Invalid;
  }
  if (header[4] > BookDailyMinutes::FILE_VERSION) {
    file.close();
    return ReadResult::NewerFormat;
  }
  if (out == nullptr || size > MAX_FILE_SIZE) {
    file.close();
    return size <= MAX_FILE_SIZE ? ReadResult::Ok : ReadResult::Invalid;
  }
  // At most ~2.4 KB, read once per detail-page open or session commit.
  auto buffer = makeUniqueNoThrow<uint8_t[]>(size);
  if (!buffer) {
    file.close();
    LOG_ERR(LOG_TAG, "Out of memory reading %s", path.c_str());
    return ReadResult::Invalid;
  }
  memcpy(buffer.get(), header, sizeof(header));
  const size_t rest = size - sizeof(header);
  const int n = rest == 0 ? 0 : file.read(buffer.get() + sizeof(header), rest);
  file.close();
  if (n != static_cast<int>(rest)) return ReadResult::Invalid;
  return out->deserialize(buffer.get(), size) ? ReadResult::Ok : ReadResult::Invalid;
}

std::string pathForCache(const std::string& cachePath) { return BookDailyMinutes::pathFor(cachePath); }

bool decodesAt(const char* path) { return readFile(path, nullptr) == ReadResult::Ok; }
}  // namespace

std::string BookDailyMinutes::pathFor(const std::string& cachePath) { return cachePath + FILE_NAME; }

void BookDailyMinutes::recordSpan(const ReadingStatsDateTime& localStart, const uint32_t seconds) {
  if (!localStart.isValid() || seconds == 0) return;
  ReadingStatsDateTime cursor = localStart;
  uint32_t remaining = seconds;
  while (remaining > 0) {
    const uint32_t dayIndex = readingStatsDayIndex(cursor.date);
    const uint32_t secondsUntilMidnight =
        (24u * 3600u) - (static_cast<uint32_t>(cursor.hour) * 3600u + static_cast<uint32_t>(cursor.minute) * 60u +
                         static_cast<uint32_t>(cursor.second));
    const uint32_t segment = std::min(remaining, secondsUntilMidnight);
    remaining -= segment;
    const uint32_t minutesToAdd = (segment + 30u) / 60u;

    auto it = std::lower_bound(days.begin(), days.end(), dayIndex,
                               [](const Day& d, const uint32_t value) { return d.day < value; });
    if (it == days.end() || it->day != dayIndex) it = days.insert(it, Day{dayIndex, 0});
    it->minutes = static_cast<uint16_t>(
        std::min<uint32_t>(MAX_MINUTES_PER_DAY, static_cast<uint32_t>(it->minutes) + minutesToAdd));
    addSecondsToReadingStatsDateTime(cursor, segment);
  }
  if (days.size() > MAX_DAYS) days.erase(days.begin(), days.begin() + static_cast<ptrdiff_t>(days.size() - MAX_DAYS));
}

uint16_t BookDailyMinutes::minutesOn(const uint32_t dayIndex) const {
  const auto it = std::lower_bound(days.begin(), days.end(), dayIndex,
                                   [](const Day& d, const uint32_t value) { return d.day < value; });
  return it != days.end() && it->day == dayIndex ? it->minutes : 0;
}

void BookDailyMinutes::serialize(uint8_t* out) const {
  memcpy(out, MAGIC, sizeof(MAGIC));
  out[4] = FILE_VERSION;
  out[5] = 0;
  writeLe16(out + 6, static_cast<uint16_t>(days.size()));
  uint8_t* entry = out + HEADER_SIZE;
  for (const Day& d : days) {
    writeLe32(entry, d.day);
    writeLe16(entry + 4, d.minutes);
    entry += ENTRY_SIZE;
  }
}

bool BookDailyMinutes::deserialize(const uint8_t* data, const size_t size, bool* newerVersion) {
  if (newerVersion) *newerVersion = false;
  if (size < HEADER_SIZE || memcmp(data, MAGIC, sizeof(MAGIC)) != 0) return false;
  if (data[4] > FILE_VERSION) {
    if (newerVersion) *newerVersion = true;
    return false;
  }
  const uint16_t count = readLe16(data + 6);
  if (data[4] != FILE_VERSION || count > MAX_DAYS || size != HEADER_SIZE + count * ENTRY_SIZE) return false;
  std::vector<Day> decoded;
  decoded.reserve(count);
  const uint8_t* entry = data + HEADER_SIZE;
  for (uint16_t i = 0; i < count; ++i, entry += ENTRY_SIZE) {
    const Day d{readLe32(entry), std::min(readLe16(entry + 4), MAX_MINUTES_PER_DAY)};
    if (!decoded.empty() && d.day <= decoded.back().day) return false;  // must be strictly ascending
    decoded.push_back(d);
  }
  days = std::move(decoded);
  return true;
}

BookDailyMinutes BookDailyMinutes::load(const std::string& cachePath) {
  BookDailyMinutes result;
  if (cachePath.empty()) return result;
  const std::string path = pathForCache(cachePath);
  if (readFile(path, &result) == ReadResult::Ok) return result;
  result.days.clear();
  if (readFile(path + ".bak", &result) == ReadResult::Ok) {
    LOG_DBG(LOG_TAG, "Recovered %s from backup", path.c_str());
    return result;
  }
  result.days.clear();
  return result;
}

bool BookDailyMinutes::save(const std::string& cachePath) const {
  if (cachePath.empty()) return false;
  const std::string path = pathForCache(cachePath);
  if (readFile(path, nullptr) == ReadResult::NewerFormat ||
      readFile(path + ".bak", nullptr) == ReadResult::NewerFormat) {
    LOG_ERR(LOG_TAG, "Refusing to overwrite newer-format %s", path.c_str());
    return false;
  }
  const size_t size = encodedSize();
  auto buffer = makeUniqueNoThrow<uint8_t[]>(size);
  if (!buffer) {
    LOG_ERR(LOG_TAG, "Out of memory writing %s", path.c_str());
    return false;
  }
  serialize(buffer.get());
  return StatsSidecarFile::writeAtomically(LOG_TAG, path.c_str(), buffer.get(), size, decodesAt);
}

void BookDailyMinutes::recordSpanAndSave(const std::string& cachePath, const ReadingStatsDateTime& localStart,
                                         const uint32_t seconds) {
  if (cachePath.empty() || !localStart.isValid() || seconds == 0) return;
  BookDailyMinutes book = load(cachePath);
  book.recordSpan(localStart, seconds);
  if (!book.save(cachePath)) LOG_ERR(LOG_TAG, "Could not save per-book reading minutes");
}
