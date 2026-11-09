#include "probe_scene.h"
#include "mmx_renderer.h"
#include <string.h>

static Ppu ppu;
static uint8_t ram[0x20000];
static uint32_t stock[256 * 224];

static void pixel(unsigned base, unsigned x, unsigned y, unsigned color) {
  unsigned bit = 7 - x;
  ppu.vram[base + y] |= ((color & 1) << bit) | (((color >> 1) & 1) << (bit + 8));
  ppu.vram[base + y + 8] |= (((color >> 2) & 1) << bit) | (((color >> 3) & 1) << (bit + 8));
}

void MmxProbeInit(void) {
  memset(&ppu, 0, sizeof(ppu));
  memset(ram, 0, sizeof(ram));
  ram[0xd1] = 2; ram[0xd2] = 4; ram[0xd3] = 4;
  ppu.inidisp = 15; ppu.bgmode = 1;
  ppu.screenEnabled[0] = 0x13; /* BG1, BG2, OBJ. */
  ppu.bgXsc[0] = 0x20; ppu.bgXsc[1] = 0x24;
  ppu.obsel = 3; /* OBJ tiles at 0x6000, separate from BG tilemaps. */
  ppu.cgram[0] = 2 | (3 << 5) | (7 << 10);
  ppu.cgram[1] = 20 | (19 << 5) | (14 << 10);
  ppu.cgram[2] = 11 | (12 << 5) | (12 << 10);
  ppu.cgram[17] = 4 | (8 << 5) | (13 << 10);
  ppu.cgram[18] = 5 | (15 << 5) | (19 << 10);
  ppu.cgram[19] = 18 | (25 << 5) | (29 << 10);
  ppu.cgram[129] = 27 | (30 << 5) | (31 << 10);
  ppu.cgram[130] = 7 | (19 << 5) | (31 << 10);
  for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 8; ++x) {
    pixel(16, x, y, y < 2 ? 1 : 2); /* Front platform. */
    pixel(32, x, y, 1); /* Distant wall. */
    pixel(48, x, y, (x >= 2 && x <= 4 && y >= 2 && y <= 4) ? 3 : 2);
    /* Four tiles make an original 16px geometric actor. */
    for (unsigned t = 0; t < 4; ++t) {
      unsigned tx = t & 1, ty = t >> 1;
      unsigned base = 0x6000 + (tx + ty * 16) * 16;
      unsigned color = (x == (tx ? 7 : 0) || y == (ty ? 7 : 0)) ? 2 : 1;
      pixel(base, x, y, color);
    }
  }
  for (unsigned y = 0; y < 32; ++y) for (unsigned x = 0; x < 32; ++x) {
    ppu.vram[0x2000 + y * 32 + x] = y >= 26 || (x == 23 && y >= 18) ? 1 : 0;
    unsigned height = 5 + ((x / 3 * 7) % 13);
    ppu.vram[0x2400 + y * 32 + x] = y >= 26 - height ? (0x400 | (x % 3 ? 3 : 2)) : 0;
  }
  for (unsigned i = 0; i < 128; ++i) ppu.oam[i * 2] = 0xe000;
  ppu.oam[0] = 0x1808; ppu.oam[1] = 0x3000;
  ppu.highOam[0] = 2; /* Fixed 16px HUD marker. */
  ppu.oam[33] = 0x3000; ppu.highOam[4] = 2;
  MmxRendererReset(); MmxRendererSetRom(NULL, 0);
  g_mmx_custom_renderer = true;
  g_mmx_render_asset_repairs = false;
}

bool MmxProbeCapture(int actor_x, unsigned scroll) {
  if (actor_x < 0) actor_x = 0;
  if (actor_x > 240) actor_x = 240;
  ppu.oam[32] = (uint16_t)(192 * 256 + actor_x);
  ppu.hScroll[1] = (uint16_t)(scroll & 255);
  MmxRendererBeginFrame(ram);
  for (unsigned y = 1; y <= 224; ++y) MmxRendererCaptureLine(&ppu, y);
  return MmxRendererEndFrame(stock);
}
