#pragma once

#include <cstdint>

// Host stub: only the clock offset is read by ReadingStatsUtils.cpp (48 = UTC).
struct CrossPointSettings {
  uint8_t clockUtcOffsetQ = 48;
};

inline CrossPointSettings SETTINGS;
