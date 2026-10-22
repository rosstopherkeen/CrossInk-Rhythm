#include "SleepArtGenerator.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"
#include "images/SleepMaskReadResponsibly.h"
#include "util/ReactionDiffusion.h"

#include <Arduino.h>

namespace SleepArtGenerator {
namespace {

constexpr char MODULE[] = "RDS";
constexpr char CACHE_PATH[] = "/.crosspoint/sleepart.bin";
constexpr char CACHE_TMP[] = "/.crosspoint/sleepart.tmp";

// Bumped whenever the header or payload layout changes, so a cache written by
// an older build is rejected rather than drawn as noise.
constexpr uint32_t CACHE_MAGIC = 0x41535043;  // "CPSA"
constexpr uint16_t CACHE_VERSION = 1;

struct CacheHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t width;
  uint16_t height;
  uint16_t preset;
  uint32_t seed;
};

// Grid size is a capability decision, not a style one: the pattern's feature
// size is fixed in grid cells, so a coarser grid means fewer, fatter features
// for the same rates. PSRAM devices can afford the grid that matches the
// reference look; the others get a coarser but still legible one.
constexpr int PSRAM_GRID_WIDTH = 200;
constexpr int PSRAM_GRID_HEIGHT = 400;
constexpr int SMALL_GRID_WIDTH = 100;
constexpr int SMALL_GRID_HEIGHT = 200;

// Generating while a book is open must never be what pushes the device over.
// Skip the run instead and try again on the next idle stretch.
constexpr size_t INTERNAL_HEAP_HEADROOM = 48u * 1024u;

constexpr int MASK_ROW_BYTES = (SLEEP_MASK_READ_RESPONSIBLY_WIDTH + 7) / 8;

// The mask is stored run-length encoded, so unpack one row at a time. Both
// callers walk rows in order, which is all the access this needs.
void decodeMaskRow(const int maskY, uint8_t* out) {
  std::memset(out, 0, MASK_ROW_BYTES);
  const int clamped = std::clamp(maskY, 0, SLEEP_MASK_READ_RESPONSIBLY_HEIGHT - 1);
  const uint16_t begin = SLEEP_MASK_READ_RESPONSIBLY_ROWS[clamped];
  const uint16_t end = SLEEP_MASK_READ_RESPONSIBLY_ROWS[clamped + 1];

  int x = 0;
  bool inside = false;  // runs alternate, starting outside the mask
  for (uint16_t i = begin; i < end && x < SLEEP_MASK_READ_RESPONSIBLY_WIDTH; ++i) {
    const int run = SLEEP_MASK_READ_RESPONSIBLY_RUNS[i];
    if (inside) {
      const int last = std::min(x + run, SLEEP_MASK_READ_RESPONSIBLY_WIDTH);
      for (int bit = x; bit < last; ++bit) out[bit >> 3] |= 0x80 >> (bit & 7);
    }
    x += run;
    inside = !inside;
  }
}

bool rowBit(const uint8_t* row, const int x) { return (row[x >> 3] >> (7 - (x & 7))) & 1u; }

// Nearest-neighbour source row for an output row, so the mask fits whatever
// logical size the panel reports.
int maskRowFor(const int y, const int height) {
  if (height <= 0) return 0;
  const int my = static_cast<int>((static_cast<int64_t>(y) * SLEEP_MASK_READ_RESPONSIBLY_HEIGHT) / height);
  return std::min(my, SLEEP_MASK_READ_RESPONSIBLY_HEIGHT - 1);
}

int maskColumnFor(const int x, const int width) {
  if (width <= 0) return 0;
  const int mx = static_cast<int>((static_cast<int64_t>(x) * SLEEP_MASK_READ_RESPONSIBLY_WIDTH) / width);
  return std::min(mx, SLEEP_MASK_READ_RESPONSIBLY_WIDTH - 1);
}

uint32_t nextSeed() {
  static uint32_t counter = 0;
  ++counter;
  return millis() * 2654435761u + counter * 2246822519u;
}

// Work in progress. Held between slices and dropped by abandon().
struct Job {
  HeapByteBuffer storage;  // a, b and the style map in one allocation
  int16_t* a = nullptr;
  int16_t* b = nullptr;
  uint8_t* styleMap = nullptr;
  int16_t* scratch = nullptr;
  int gridWidth = 0;
  int gridHeight = 0;
  int screenWidth = 0;
  int screenHeight = 0;
  int stepsDone = 0;
  size_t presetIndex = 0;
  uint32_t seed = 0;
  bool active = false;

  ReactionDiffusion::Field field() const {
    return ReactionDiffusion::Field{a, b, styleMap, gridWidth, gridHeight};
  }
};

Job job;

// needsWork() runs from the main loop, so the cache check must not be an SD
// stat every iteration. -1 means not looked at yet since boot; the file only
// changes through this module, so the cached answer stays correct.
int8_t cachePresent = -1;

size_t presetForSetting() {
  const uint8_t choice = SETTINGS.sleepScreenSimulation;
  if (choice == CrossPointSettings::SIM_RANDOM || choice > CrossPointSettings::SIM_CHAOS) {
    return static_cast<size_t>(nextSeed() % ReactionDiffusion::PRESET_COUNT);
  }
  return static_cast<size_t>(choice - CrossPointSettings::SIM_USKATE);
}

bool reactionWallpaperSelected() {
  return SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::REACTION_DIFFUSION;
}

bool startJob(const int screenWidth, const int screenHeight) {
  const bool psram = psramHeapAvailable();
  const int gw = psram ? PSRAM_GRID_WIDTH : SMALL_GRID_WIDTH;
  const int gh = psram ? PSRAM_GRID_HEIGHT : SMALL_GRID_HEIGHT;

  const size_t cells = static_cast<size_t>(gw) * gh;
  const size_t planeBytes = cells * sizeof(int16_t);
  const size_t styleRowBytes = static_cast<size_t>(gw + 7) / 8u;
  const size_t styleBytes = styleRowBytes * gh;
  const size_t scratchBytes = ReactionDiffusion::scratchCount(gw) * sizeof(int16_t);
  const size_t total = planeBytes * 2 + styleBytes + scratchBytes;

  if (psram) {
    job.storage = makePsramByteBufferNoThrow(total);
  } else {
    const ByteHeapSnapshot heap = byteHeapSnapshot(MemoryPool::Internal);
    if (heap.largest < total + INTERNAL_HEAP_HEADROOM) {
      LOG_DBG(MODULE, "Deferring sleep art: need %u, largest block %u", static_cast<unsigned>(total),
              static_cast<unsigned>(heap.largest));
      return false;
    }
    job.storage = makeHeapByteBufferNoThrow(total);
  }
  if (!job.storage) {
    LOG_ERR(MODULE, "Could not allocate %u bytes for the sleep art grid", static_cast<unsigned>(total));
    return false;
  }

  uint8_t* base = job.storage.get();
  job.a = reinterpret_cast<int16_t*>(base);
  job.b = reinterpret_cast<int16_t*>(base + planeBytes);
  job.scratch = reinterpret_cast<int16_t*>(base + planeBytes * 2);
  job.styleMap = base + planeBytes * 2 + scratchBytes;
  job.gridWidth = gw;
  job.gridHeight = gh;
  job.screenWidth = screenWidth;
  job.screenHeight = screenHeight;
  job.stepsDone = 0;
  job.presetIndex = presetForSetting();
  job.seed = nextSeed();
  job.active = true;

  // Downsample the mask into the style map once, so the inner loop only does a
  // bit test rather than a rescale.
  std::memset(job.styleMap, 0, styleBytes);
  uint8_t maskLine[MASK_ROW_BYTES];
  int decodedRow = -1;
  for (int y = 0; y < gh; ++y) {
    const int sourceRow = maskRowFor(y, gh);
    if (sourceRow != decodedRow) {
      decodeMaskRow(sourceRow, maskLine);
      decodedRow = sourceRow;
    }
    for (int x = 0; x < gw; ++x) {
      if (!rowBit(maskLine, maskColumnFor(x, gw))) continue;
      job.styleMap[static_cast<size_t>(y) * styleRowBytes + (x >> 3)] |= 0x80 >> (x & 7);
    }
  }

  ReactionDiffusion::seed(job.field(), job.seed);
  LOG_INF(MODULE, "Sleep art started: grid %dx%d preset %u seed %lu (%u bytes %s)", gw, gh,
          static_cast<unsigned>(job.presetIndex), static_cast<unsigned long>(job.seed), static_cast<unsigned>(total),
          psram ? "PSRAM" : "internal");
  return true;
}

// Writes the finished field as packed 1-bit rows at panel resolution.
bool writeCache() {
  const ReactionDiffusion::Preset& settings = ReactionDiffusion::preset(job.presetIndex);
  const ReactionDiffusion::Field field = job.field();
  const size_t rowBytes = static_cast<size_t>(job.screenWidth + 7) / 8u;

  auto row = makeUniqueNoThrow<uint8_t[]>(rowBytes);
  if (!row) {
    LOG_ERR(MODULE, "Could not allocate the sleep art row buffer");
    return false;
  }

  HalFile file;
  if (!Storage.openFileForWrite(MODULE, CACHE_TMP, file)) {
    LOG_ERR(MODULE, "Could not open %s", CACHE_TMP);
    return false;
  }

  const CacheHeader header{CACHE_MAGIC,
                           CACHE_VERSION,
                           static_cast<uint16_t>(job.screenWidth),
                           static_cast<uint16_t>(job.screenHeight),
                           static_cast<uint16_t>(job.presetIndex),
                           job.seed};
  bool ok = file.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header)) == sizeof(header);
  for (int y = 0; ok && y < job.screenHeight; ++y) {
    std::memset(row.get(), 0, rowBytes);
    for (int x = 0; x < job.screenWidth; ++x) {
      if (ReactionDiffusion::sampleB(field, x, job.screenWidth, y, job.screenHeight) <= settings.thresholdQ14) continue;
      row[x >> 3] |= 0x80 >> (x & 7);
    }
    ok = file.write(row.get(), rowBytes) == rowBytes;
  }
  file.close();

  if (!ok) {
    LOG_ERR(MODULE, "Could not write the sleep art cache");
    Storage.remove(CACHE_TMP);
    return false;
  }

  // Rename last so a half-written file is never picked up at sleep.
  Storage.remove(CACHE_PATH);
  if (!Storage.rename(CACHE_TMP, CACHE_PATH)) {
    LOG_ERR(MODULE, "Could not rename the sleep art cache into place");
    Storage.remove(CACHE_TMP);
    return false;
  }
  cachePresent = 1;
  LOG_INF(MODULE, "Sleep art ready: preset %u seed %lu", static_cast<unsigned>(job.presetIndex),
          static_cast<unsigned long>(job.seed));
  return true;
}

bool readHeader(HalFile& file, CacheHeader& header) {
  const int got = file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header));
  return got == static_cast<int>(sizeof(header)) && header.magic == CACHE_MAGIC && header.version == CACHE_VERSION;
}

}  // namespace

void abandon() {
  if (!job.active) return;
  job = Job{};
}

bool needsWork() {
  if (!reactionWallpaperSelected()) {
    abandon();
    return false;
  }
  if (job.active) return true;
  if (cachePresent < 0) cachePresent = Storage.exists(CACHE_PATH) ? 1 : 0;
  return cachePresent == 0;
}

bool runSlice(const int screenWidth, const int screenHeight, const uint32_t budgetMs) {
  if (!reactionWallpaperSelected()) {
    abandon();
    return false;
  }

  // A rotation between slices would make the half-built image the wrong shape.
  if (job.active && (job.screenWidth != screenWidth || job.screenHeight != screenHeight)) abandon();
  if (!job.active && !startJob(screenWidth, screenHeight)) return false;

  const ReactionDiffusion::Preset& settings = ReactionDiffusion::preset(job.presetIndex);
  const uint32_t deadline = millis() + budgetMs;
  // One step per clock check. A single step already costs milliseconds over the
  // PSRAM grid, so batching steps would overshoot the slice budget and show up
  // as input lag.
  while (job.stepsDone < settings.steps) {
    ReactionDiffusion::advance(job.field(), settings, 1, job.scratch);
    ++job.stepsDone;
    if (static_cast<int32_t>(millis() - deadline) >= 0) break;
  }

  if (job.stepsDone < settings.steps) return false;

  const bool written = writeCache();
  abandon();
  return written;
}

bool cacheIsReady(const int screenWidth, const int screenHeight, size_t& outBytes) {
  outBytes = 0;
  HalFile file;
  if (!Storage.openFileForRead(MODULE, CACHE_PATH, file)) return false;
  CacheHeader header{};
  const bool ok = readHeader(file, header) && header.width == screenWidth && header.height == screenHeight;
  file.close();
  if (!ok) return false;
  outBytes = (static_cast<size_t>(screenWidth + 7) / 8u) * static_cast<size_t>(screenHeight);
  return true;
}

bool readCache(const int screenWidth, const int screenHeight, const RowCallback rowCallback, void* ctx) {
  if (rowCallback == nullptr) return false;

  HalFile file;
  if (!Storage.openFileForRead(MODULE, CACHE_PATH, file)) return false;
  CacheHeader header{};
  if (!readHeader(file, header) || header.width != screenWidth || header.height != screenHeight) {
    file.close();
    // Drop it rather than leaving a file that needsWork() would keep treating
    // as done, which would stop the wallpaper regenerating for good.
    LOG_ERR(MODULE, "Sleep art cache does not match this panel; discarding it");
    clearCache();
    return false;
  }

  const size_t rowBytes = static_cast<size_t>(screenWidth + 7) / 8u;
  auto row = makeUniqueNoThrow<uint8_t[]>(rowBytes);
  if (!row) {
    file.close();
    LOG_ERR(MODULE, "Could not allocate the sleep art row buffer");
    return false;
  }

  bool ok = true;
  for (int y = 0; y < screenHeight && ok; ++y) {
    ok = file.read(row.get(), rowBytes) == static_cast<int>(rowBytes);
    if (ok) rowCallback(y, row.get(), rowBytes, ctx);
  }
  file.close();
  if (!ok) LOG_ERR(MODULE, "Sleep art cache ended early");
  return ok;
}

void clearCache() {
  Storage.remove(CACHE_PATH);
  cachePresent = 0;
}

void maskRow(const int y, const int screenWidth, const int screenHeight, uint8_t* row, const size_t rowBytes) {
  std::memset(row, 0, rowBytes);
  uint8_t maskLine[MASK_ROW_BYTES];
  decodeMaskRow(maskRowFor(y, screenHeight), maskLine);
  for (int x = 0; x < screenWidth; ++x) {
    if (rowBit(maskLine, maskColumnFor(x, screenWidth))) row[x >> 3] |= 0x80 >> (x & 7);
  }
}

}  // namespace SleepArtGenerator
