// Daily reading minutes sidecar. Recording rule ported from XPoint
// (https://github.com/Belphemur/XPoint, MIT): ReadingStatsUtils.cpp
// recordReadingSpanIntoHistory() minute accumulation.
#include "DailyMinutesLog.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

#include "StatsSidecarFile.h"

namespace {
constexpr char LOG_TAG[] = "DMIN";
constexpr char DAILY_MINUTES_PATH[] = "/.crosspoint/daily_minutes.bin";
constexpr char DAILY_MINUTES_BAK_PATH[] = "/.crosspoint/daily_minutes.bin.bak";
constexpr uint8_t MAGIC[4] = {'C', 'I', 'D', 'M'};
constexpr size_t HEADER_SIZE = 16;
constexpr size_t REAL_OFFSET = HEADER_SIZE;
constexpr size_t ESTIMATED_OFFSET = REAL_OFFSET + DailyMinutesLog::WINDOW_DAYS * 2;

// Latched when a newer-format file is seen so this build never clobbers it.
bool s_forwardVersionSeen = false;

void writeLe16(uint8_t* out, const uint16_t value) {
  out[0] = static_cast<uint8_t>(value & 0xFF);
  out[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

void writeLe32(uint8_t* out, const uint32_t value) {
  out[0] = static_cast<uint8_t>(value & 0xFF);
  out[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  out[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
  out[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

uint16_t readLe16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) | static_cast<uint16_t>(static_cast<uint16_t>(data[1]) << 8);
}

uint32_t readLe32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

void shiftOlder(std::array<uint16_t, DailyMinutesLog::WINDOW_DAYS>& minutes, const uint32_t shiftDays) {
  if (shiftDays == 0) return;
  if (shiftDays >= minutes.size()) {
    minutes.fill(0);
    return;
  }
  for (size_t i = minutes.size(); i-- > shiftDays;) {
    minutes[i] = minutes[i - shiftDays];
  }
  std::fill(minutes.begin(), minutes.begin() + shiftDays, 0);
}

uint16_t addClamped(const uint16_t current, const uint32_t add) {
  const uint32_t sum = static_cast<uint32_t>(current) + add;
  return static_cast<uint16_t>(std::min<uint32_t>(sum, DailyMinutesLog::MAX_MINUTES_PER_DAY));
}

// Reads the whole fixed-size file. Returns false for missing, short or long files.
bool readFile(const char* path, uint8_t* out, bool* newerVersion) {
  if (!Storage.exists(path)) return false;
  HalFile file;
  if (!Storage.openFileForRead(LOG_TAG, path, file)) return false;
  const size_t size = file.fileSize();
  // Peek the header first so a longer future-version file still latches the guard.
  const size_t toRead = std::min(size, DailyMinutesLog::FILE_SIZE);
  const int n = file.read(out, toRead);
  file.close();
  if (n != static_cast<int>(toRead) || toRead < HEADER_SIZE) return false;
  if (memcmp(out, MAGIC, sizeof(MAGIC)) == 0 && out[4] > DailyMinutesLog::FILE_VERSION) {
    if (newerVersion) *newerVersion = true;
    return false;
  }
  return size == DailyMinutesLog::FILE_SIZE;
}

bool headerIsCurrent(const uint8_t* header) {
  return memcmp(header, MAGIC, sizeof(MAGIC)) == 0 && header[4] == DailyMinutesLog::FILE_VERSION &&
         readLe16(header + 6) == DailyMinutesLog::WINDOW_DAYS;
}

// Cheap validity probe for backup rotation: size plus a 16-byte header read.
bool decodesAt(const char* path) {
  if (!Storage.exists(path)) return false;
  HalFile file;
  if (!Storage.openFileForRead(LOG_TAG, path, file)) return false;
  uint8_t header[HEADER_SIZE];
  const bool ok = file.fileSize() == DailyMinutesLog::FILE_SIZE &&
                  file.read(header, sizeof(header)) == static_cast<int>(sizeof(header)) && headerIsCurrent(header);
  file.close();
  return ok;
}
}  // namespace

bool DailyMinutesLog::slotForDay(const uint32_t dayIndex, size_t& slot) const {
  if (isEmpty() || dayIndex > anchorDay) return false;
  const uint32_t offset = anchorDay - dayIndex;
  if (offset >= WINDOW_DAYS) return false;
  slot = offset;
  return true;
}

void DailyMinutesLog::clear() {
  flags = 0;
  anchorDay = 0;
  firstRealDay = 0;
  realMinutes.fill(0);
  estimatedMinutes.fill(0);
}

void DailyMinutesLog::advanceTo(const uint32_t dayIndex) {
  if (isEmpty()) {
    realMinutes.fill(0);
    estimatedMinutes.fill(0);
    anchorDay = dayIndex;
    return;
  }
  if (dayIndex <= anchorDay) return;
  const uint32_t shift = dayIndex - anchorDay;
  shiftOlder(realMinutes, shift);
  shiftOlder(estimatedMinutes, shift);
  anchorDay = dayIndex;
}

void DailyMinutesLog::recordSpan(const ReadingStatsDateTime& localStart, const uint32_t seconds) {
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

    // Round to the nearest minute per day segment, as XPoint does.
    const uint32_t minutesToAdd = std::min<uint32_t>(MAX_MINUTES_PER_DAY, (segment + 30u) / 60u);
    advanceTo(dayIndex);
    if (firstRealDay == 0 || dayIndex < firstRealDay) firstRealDay = dayIndex;
    size_t slot = 0;
    if (slotForDay(dayIndex, slot)) {
      realMinutes[slot] = addClamped(realMinutes[slot], minutesToAdd);
    }
    addSecondsToReadingStatsDateTime(cursor, segment);
  }
}

void DailyMinutesLog::addEstimatedMinutes(const uint32_t dayIndex, const uint32_t minutes) {
  size_t slot = 0;
  if (!slotForDay(dayIndex, slot)) return;
  estimatedMinutes[slot] = addClamped(estimatedMinutes[slot], minutes);
}

uint16_t DailyMinutesLog::realMinutesOnDay(const uint32_t dayIndex) const {
  size_t slot = 0;
  return slotForDay(dayIndex, slot) ? realMinutes[slot] : 0;
}

uint16_t DailyMinutesLog::estimatedMinutesOnDay(const uint32_t dayIndex) const {
  size_t slot = 0;
  return slotForDay(dayIndex, slot) ? estimatedMinutes[slot] : 0;
}

uint16_t DailyMinutesLog::minutesOnDay(const uint32_t dayIndex, bool* estimated) const {
  if (estimated) *estimated = false;
  const uint16_t real = realMinutesOnDay(dayIndex);
  if (real > 0) return real;
  const uint16_t estimate = estimatedMinutesOnDay(dayIndex);
  if (estimate > 0 && estimated) *estimated = true;
  return estimate;
}

void DailyMinutesLog::serialize(uint8_t* out) const {
  memset(out, 0, FILE_SIZE);
  memcpy(out, MAGIC, sizeof(MAGIC));
  out[4] = FILE_VERSION;
  out[5] = flags;
  writeLe16(out + 6, static_cast<uint16_t>(WINDOW_DAYS));
  writeLe32(out + 8, anchorDay);
  writeLe32(out + 12, firstRealDay);
  for (size_t i = 0; i < WINDOW_DAYS; ++i) {
    writeLe16(out + REAL_OFFSET + i * 2, realMinutes[i]);
    writeLe16(out + ESTIMATED_OFFSET + i * 2, estimatedMinutes[i]);
  }
}

bool DailyMinutesLog::deserialize(const uint8_t* data, const size_t size, bool* newerVersion) {
  if (newerVersion) *newerVersion = false;
  if (size < HEADER_SIZE || memcmp(data, MAGIC, sizeof(MAGIC)) != 0) return false;
  if (data[4] > FILE_VERSION) {
    if (newerVersion) *newerVersion = true;
    return false;
  }
  if (size != FILE_SIZE || !headerIsCurrent(data)) return false;

  // Fully validated above, so decode in place without a 372-byte temporary.
  flags = data[5];
  anchorDay = readLe32(data + 8);
  firstRealDay = readLe32(data + 12);
  for (size_t i = 0; i < WINDOW_DAYS; ++i) {
    realMinutes[i] = std::min(readLe16(data + REAL_OFFSET + i * 2), MAX_MINUTES_PER_DAY);
    estimatedMinutes[i] = std::min(readLe16(data + ESTIMATED_OFFSET + i * 2), MAX_MINUTES_PER_DAY);
  }
  return true;
}

bool DailyMinutesLog::loadInto(DailyMinutesLog& out) {
  // 380-byte read buffer: one fixed-size file, read once per session commit or
  // screen open; the caller's log is decoded in place to avoid a second copy.
  uint8_t data[FILE_SIZE];
  bool newer = false;
  if (readFile(DAILY_MINUTES_PATH, data, &newer) && out.deserialize(data, sizeof(data))) return true;
  if (newer) {
    s_forwardVersionSeen = true;
    LOG_ERR(LOG_TAG, "daily_minutes.bin is from a newer build; refusing to overwrite");
    out.clear();
    return false;
  }
  newer = false;
  if (readFile(DAILY_MINUTES_BAK_PATH, data, &newer) && out.deserialize(data, sizeof(data))) {
    LOG_DBG(LOG_TAG, "Recovered daily minutes from backup");
    return true;
  }
  if (newer) {
    s_forwardVersionSeen = true;
    LOG_ERR(LOG_TAG, "daily_minutes.bin.bak is from a newer build; refusing to overwrite");
  }
  out.clear();
  return false;
}

DailyMinutesLog DailyMinutesLog::load() {
  DailyMinutesLog log;
  loadInto(log);
  return log;
}

bool DailyMinutesLog::save() const {
  if (s_forwardVersionSeen) {
    LOG_ERR(LOG_TAG, "Refusing to overwrite newer-format daily minutes file");
    return false;
  }
  // 380-byte stack buffer; the log itself is not on this frame (see recordSpanAndSave).
  uint8_t data[FILE_SIZE];
  serialize(data);
  return StatsSidecarFile::writeAtomically(LOG_TAG, DAILY_MINUTES_PATH, data, sizeof(data), decodesAt);
}

void DailyMinutesLog::recordSpanAndSave(const ReadingStatsDateTime& localStart, const uint32_t seconds) {
  if (!localStart.isValid() || seconds == 0) return;
  // Heap, not stack: this runs at reader exit beneath the stats save chain, and
  // the 372-byte log plus the 380-byte file buffer would otherwise share a frame.
  auto log = makeUniqueNoThrow<DailyMinutesLog>();
  if (!log) {
    LOG_ERR(LOG_TAG, "Out of memory recording daily minutes");
    return;
  }
  loadInto(*log);
  log->recordSpan(localStart, seconds);
  if (!log->save()) {
    LOG_ERR(LOG_TAG, "Could not save daily reading minutes");
  }
}

#ifdef READING_RHYTHM_TEST
void DailyMinutesLog::resetForwardGuardForTest() { s_forwardVersionSeen = false; }
#endif
