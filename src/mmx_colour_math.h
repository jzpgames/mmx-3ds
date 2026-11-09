#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Three 5-bit SNES components in 6-bit lanes, leaving an independent
 * carry/borrow bit for each channel. No host colour-space conversion. */
static inline uint32_t MmxColourLanes(uint16_t snes) {
  return (snes & 31u) | ((snes & 0x3e0u) << 1) | ((snes & 0x7c00u) << 2);
}
static inline uint32_t MmxColourMixLanes(uint32_t a, uint32_t b,
    bool subtract, bool half, const uint8_t brightness[32]) {
  const uint32_t values = 0x1f7dfu, guards = 0x20820u;
  uint32_t mixed;
  if (subtract) {
    mixed = (a | guards) - b;
    uint32_t nonnegative = mixed & guards;
    mixed &= nonnegative - (nonnegative >> 5);
    if (half) mixed = (mixed >> 1) & values;
  } else {
    mixed = a + b;
    if (half) mixed = (mixed >> 1) & values;
    else {
      uint32_t overflow = mixed & guards;
      mixed = (mixed & values) | (overflow - (overflow >> 5));
    }
  }
  return (uint32_t)brightness[mixed & 31] << 16 |
      (uint32_t)brightness[(mixed >> 6) & 31] << 8 |
      brightness[(mixed >> 12) & 31];
}
