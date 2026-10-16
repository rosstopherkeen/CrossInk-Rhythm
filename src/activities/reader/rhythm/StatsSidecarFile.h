#pragma once

#include <cstddef>
#include <cstdint>

// Crash-safe writer for the Reading Rhythm / Books Finished sidecar files.
//
// These sidecars live next to global_stats.bin in /.crosspoint/ but never
// touch it: stock CrossInk ignores unknown files, so the user's existing stats
// stay readable after reflashing upstream firmware.
//
// Write order mirrors GlobalReadingStats::saveToFile():
//   1. write <path>.tmp, flush, sync, close
//   2. re-read <path>.tmp and compare every byte against the buffer
//   3. rotate <path> to <path>.bak (keeping an older valid .bak when <path>
//      itself no longer decodes)
//   4. rename <path>.tmp to <path>, restoring <path>.bak on failure
namespace StatsSidecarFile {

// Returns true when the file at path decodes as a valid sidecar. Used to decide
// whether the current primary file is worth keeping as the new backup.
using Validator = bool (*)(const char* path);

bool writeAtomically(const char* tag, const char* path, const uint8_t* data, size_t size, Validator isValid);

}  // namespace StatsSidecarFile
