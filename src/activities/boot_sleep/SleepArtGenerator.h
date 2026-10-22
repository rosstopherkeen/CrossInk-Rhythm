#pragma once

#include <cstddef>
#include <cstdint>

// Builds the Reaction sleep screen ahead of time and caches it on SD.
//
// Waking from deep sleep is a full reboot, so there is no cheap "on unlock"
// hook, and the lock path has to stay instant. The image is therefore generated
// in small slices while the device is already awake and idle, cached as a 1-bit
// screen image, and simply blitted at sleep. SleepActivity deletes the cache
// after drawing it, so the next idle period rotates in a fresh seed.
namespace SleepArtGenerator {

// True when the Reaction wallpaper is selected and no usable cache exists, so
// the main loop should start spending idle slices here.
bool needsWork();

// Runs for at most budgetMs, then returns. Returns true once the cache has been
// written. Safe to abandon part-way: the next call starts the image again.
// The size is the panel's logical size in the portrait orientation the sleep
// screen uses, which is not necessarily the orientation on screen right now.
bool runSlice(int screenWidth, int screenHeight, uint32_t budgetMs);

// Drops any work in progress, freeing the grid. Called when the user touches
// anything, and before the device sleeps.
void abandon();

// Screen image waiting to be drawn, if any. outBytes is the payload size.
bool cacheIsReady(int screenWidth, int screenHeight, size_t& outBytes);

// Reads the cached rows into `rowCallback`, one packed row at a time.
using RowCallback = void (*)(int y, const uint8_t* row, size_t rowBytes, void* ctx);
bool readCache(int screenWidth, int screenHeight, RowCallback rowCallback, void* ctx);

// Removes the cache so the next idle period generates a new image.
void clearCache();

// Packs one row of the built-in mask, scaled to the panel, for the fallback
// when no cache exists yet.
void maskRow(int y, int screenWidth, int screenHeight, uint8_t* row, size_t rowBytes);

}  // namespace SleepArtGenerator
