#include "framebuffer.h"
#include <stdint.h>
#include <string.h>

/* libctru's top framebuffer is stored column by column (the LCD is rotated):
 * byte offset ((x * 240) + (239 - y)) * 3. One output column is 720
 * contiguous bytes; memory index k inside it holds screen row 239 - k.
 *
 * Revision 3. The previous blitter walked one source column at a time.
 * Source rows are 1024 bytes apart, so a column touched only four sets of
 * the ARM11's 16 KB data cache and missed on almost every pixel. This one
 * transposes in blocks: eight source pixels (one 32-byte cache line) per row,
 * four rows at a time, written as three aligned 32-bit stores per four
 * pixels instead of twelve byte stores. Only the black borders are cleared,
 * not the whole 288 KB buffer. */
enum { COLUMN_BYTES = 240 * 3 };

/* Revision 15: "sharp bilinear" for the stretched modes. Nearest-neighbour
 * 256 -> 400 (or 320) gives columns one and two LCD pixels wide in an uneven
 * pattern; 224 -> 240 doubles one row in fifteen. Here the image is scaled
 * as if first enlarged by an integer factor (2) and then bilinearly reduced
 * to the panel: inside a source pixel the colour is flat, and only the one
 * LCD pixel straddling a boundary is a blend of the two neighbours. Every
 * source pixel then has the same apparent width and edges stay crisp. */
enum { SHARP_PRESCALE = 2 };
typedef struct SharpTap { uint16_t index; uint16_t weight; } SharpTap; /* quarters of index+1 */
void Mmx3dsSharpTaps(unsigned src, unsigned dst, SharpTap *taps);
void Mmx3dsSharpTaps(unsigned src, unsigned dst, SharpTap *taps) {
  const unsigned k = SHARP_PRESCALE;
  for (unsigned x = 0; x < dst; ++x) {
    /* Output centre in the prescaled grid, 1/256 pixel, minus half a pixel. */
    int p = (int)(((2 * x + 1) * src * k * 256) / (2 * dst)) - 128;
    if (p < 0) p = 0;
    unsigned i = (unsigned)p >> 8, f = (unsigned)p & 255;
    unsigned a = i / k, b = (i + 1) / k, w = (f + 32) >> 6; /* 0..4 */
    if (a >= src - 1) { a = src - 1; b = a; }
    if (a == b) w = 0;
    if (w == 4) { a = b; w = 0; }
    taps[x].index = (uint16_t)a;
    taps[x].weight = (uint16_t)w;
  }
}
/* Per-byte floor average: one ARMv6 UHADD8 on the console. */
static inline uint32_t avg8(uint32_t x, uint32_t y) {
#if defined(__ARM_ARCH_6K__) || defined(__ARM_ARCH_6__) || defined(__3DS__)
  uint32_t r; __asm__("uhadd8 %0, %1, %2" : "=r"(r) : "r"(x), "r"(y)); return r;
#else
  return (x & y) + (((x ^ y) & 0xfefefefeu) >> 1);
#endif
}
/* Quarter-step blend toward c2 built from averages: 1/4, 1/2, 3/4. */
static inline uint32_t blend4(uint32_t c1, uint32_t c2, unsigned w) {
  const uint32_t m = avg8(c1, c2);
  return w == 2 ? m : w == 1 ? avg8(c1, m) : avg8(m, c2);
}
static void blit_sharp(const uint32_t *image, uint8_t *framebuffer, unsigned width) {
  static SharpTap xt[2][400], yt[240];
  static unsigned built[2];
  const unsigned slot = width == 400 ? 0 : 1;
  if (built[slot] != width) { Mmx3dsSharpTaps(256, width, xt[slot]); built[slot] = width; }
  static bool ybuilt;
  if (!ybuilt) { Mmx3dsSharpTaps(224, 240, yt); ybuilt = true; }
  const unsigned left = (400 - width) / 2;
  memset(framebuffer, 0, (size_t)left * COLUMN_BYTES);
  memset(framebuffer + (size_t)(left + width) * COLUMN_BYTES, 0,
         (size_t)(400 - left - width) * COLUMN_BYTES);
  const bool aligned = !((uintptr_t)framebuffer & 3);
  /* Vertical pass in blocks of eight source columns (one cache line per
   * source row), plus the next column for edge blends; stored bottom row
   * first so each output column is a straight walk. */
  static uint32_t vcol[9][240];
  unsigned x = 0;
  for (unsigned block = 0; block < 256 && x < width; block += 8) {
    const unsigned ncols = block + 8 < 256 ? 9 : 8;
    for (unsigned y = 0; y < 240; ++y) {
      const unsigned r = yt[y].index, wv = yt[y].weight;
      const uint32_t *row = image + r * 256 + block;
      uint32_t *v = &vcol[0][239 - y];
      if (!wv) for (unsigned j = 0; j < ncols; ++j) v[j * 240] = row[j];
      else {
        const uint32_t *next = row + 256;
        for (unsigned j = 0; j < ncols; ++j) v[j * 240] = blend4(row[j], next[j], wv);
      }
    }
    for (; x < width && xt[slot][x].index < block + 8; ++x) {
    const unsigned a = xt[slot][x].index - block, w = xt[slot][x].weight;
    const uint32_t *cols[2] = {vcol[a], vcol[a + 1]};
    uint8_t *dst = framebuffer + (size_t)(left + x) * COLUMN_BYTES;
    const uint32_t *c0 = cols[0], *c1 = w ? cols[1] : NULL;
    if (aligned) {
      uint32_t *d = (uint32_t *)(void *)dst;
      for (unsigned k = 0; k < 240; k += 4) {
        uint32_t p0, p1, p2, p3;
        if (c1) {
          uint32_t m0 = avg8(c0[k], c1[k]), m1 = avg8(c0[k + 1], c1[k + 1]);
          uint32_t m2 = avg8(c0[k + 2], c1[k + 2]), m3 = avg8(c0[k + 3], c1[k + 3]);
          if (w == 1) { m0 = avg8(c0[k], m0); m1 = avg8(c0[k + 1], m1); m2 = avg8(c0[k + 2], m2); m3 = avg8(c0[k + 3], m3); }
          else if (w == 3) { m0 = avg8(m0, c1[k]); m1 = avg8(m1, c1[k + 1]); m2 = avg8(m2, c1[k + 2]); m3 = avg8(m3, c1[k + 3]); }
          p0 = m0; p1 = m1; p2 = m2; p3 = m3;
        } else {
          p0 = c0[k]; p1 = c0[k + 1]; p2 = c0[k + 2]; p3 = c0[k + 3];
        }
        *d++ = (p0 & 0xffffff) | (p1 << 24);
        *d++ = ((p1 >> 8) & 0xffff) | (p2 << 16);
        *d++ = ((p2 >> 16) & 0xff) | (p3 << 8);
      }
    } else {
      for (unsigned k = 0; k < 240; ++k) {
        uint32_t c = c1 ? blend4(c0[k], c1[k], w) : c0[k];
        dst[k * 3] = (uint8_t)c; dst[k * 3 + 1] = (uint8_t)(c >> 8); dst[k * 3 + 2] = (uint8_t)(c >> 16);
      }
    }
    }
  }
}

/* Revision 13: three display modes. Wide stretches 256x224 to the full
 * 400x240 panel, 4:3 to 320x240, pixel shows it 1:1 (256x224). Since
 * revision 15 the stretched modes use blit_sharp. */
void Mmx3dsBlitTopBgr8(const uint32_t *image, uint8_t *framebuffer, Mmx3dsDisplay mode) {
  if (mode == MMX3DS_DISPLAY_WIDE) { blit_sharp(image, framebuffer, 400); return; }
  if (mode == MMX3DS_DISPLAY_43) { blit_sharp(image, framebuffer, 320); return; }
  const bool square = mode == MMX3DS_DISPLAY_PIXEL;
  const unsigned width = square ? 256 : mode == MMX3DS_DISPLAY_WIDE ? 400 : 320;
  const unsigned height = square ? 224 : 240;
  const unsigned left = (400 - width) / 2, top = (240 - height) / 2;
  const unsigned first = 240 - top - height; /* First image byte row k. */
  unsigned offset[240];
  for (unsigned k = 0; k < height; ++k) {
    unsigned y = 239 - (first + k) - top;
    offset[k] = (square ? y : y * 14 / 15) * 256;
  }
  memset(framebuffer, 0, (size_t)left * COLUMN_BYTES);
  memset(framebuffer + (size_t)(left + width) * COLUMN_BYTES, 0,
         (size_t)(400 - left - width) * COLUMN_BYTES);
  if (first) for (unsigned x = 0; x < width; ++x) {
    uint8_t *column = framebuffer + (size_t)(left + x) * COLUMN_BYTES;
    memset(column, 0, first * 3);
    memset(column + (first + height) * 3, 0, (240 - first - height) * 3);
  }
  const bool aligned = !((uintptr_t)framebuffer & 3) && !(first & 3) && !(height & 3);
  unsigned x = 0;
  for (unsigned block = 0; block < 256; block += 8) {
    /* Output columns whose source column lies in this 8-pixel block. */
    uint8_t *columns[16]; unsigned source[16], count = 0;
    for (; x < width && count < 16; ++x) {
      unsigned sx = x * 256 / width;
      if (sx >= block + 8) break;
      columns[count] = framebuffer + (size_t)(left + x) * COLUMN_BYTES + first * 3;
      source[count++] = sx - block;
    }
    const uint32_t *base = image + block;
    if (aligned) {
      for (unsigned k = 0; k < height; k += 4) {
        const uint32_t *r0 = base + offset[k], *r1 = base + offset[k + 1];
        const uint32_t *r2 = base + offset[k + 2], *r3 = base + offset[k + 3];
        for (unsigned j = 0; j < count; ++j) {
          unsigned s = source[j];
          uint32_t a = r0[s], b = r1[s], c = r2[s], d = r3[s];
          /* Little-endian bytes B,G,R per pixel. */
          uint32_t *dst = (uint32_t *)(void *)(columns[j] + k * 3);
          dst[0] = (a & 0xffffff) | (b << 24);
          dst[1] = ((b >> 8) & 0xffff) | (c << 16);
          dst[2] = ((c >> 16) & 0xff) | (d << 8);
        }
      }
    } else {
      for (unsigned k = 0; k < height; ++k) {
        const uint32_t *row = base + offset[k];
        for (unsigned j = 0; j < count; ++j) {
          uint32_t rgb = row[source[j]];
          uint8_t *dst = columns[j] + k * 3;
          dst[0] = (uint8_t)rgb; dst[1] = (uint8_t)(rgb >> 8); dst[2] = (uint8_t)(rgb >> 16);
        }
      }
    }
  }
}
