#pragma once

#include <cstdint>

// Host stub: tests set the UTC date/time the reading-stats helpers see.
class HalClock {
 public:
  bool available = false;
  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
  uint8_t hour = 0;
  uint8_t minute = 0;

  bool isAvailable() const { return available; }
  bool getDateTime(uint16_t& y, uint8_t& mo, uint8_t& d, uint8_t& h, uint8_t& mi) const {
    if (!available) return false;
    y = year;
    mo = month;
    d = day;
    h = hour;
    mi = minute;
    return true;
  }
};

inline HalClock halClock;
