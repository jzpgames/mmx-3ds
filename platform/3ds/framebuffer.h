#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum Mmx3dsDisplay {
  MMX3DS_DISPLAY_WIDE, MMX3DS_DISPLAY_43, MMX3DS_DISPLAY_PIXEL, MMX3DS_DISPLAY_COUNT
} Mmx3dsDisplay;
/* Write libctru's rotated 400x240 BGR8 framebuffer from native 256x224 RGB. */
void Mmx3dsBlitTopBgr8(const uint32_t *image, uint8_t *framebuffer, Mmx3dsDisplay mode);
