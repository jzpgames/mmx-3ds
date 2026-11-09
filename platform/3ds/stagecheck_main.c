/* Host diagnostic: load stage STAGE (poking $1F7A during level setup), play a
 * little, and report how much of each presented pair actually has stereo
 * disparity, plus the window/BG2 registers that decide it. Saves PPMs. */
#include "game_core.h"
#include "mmx_renderer.h"
#include "mmx_wide_policy.h"
#include "common_rtl.h"
#include "snes/ppu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t eyes[256 * 224 * 2];
static void save(const char *path, const uint32_t *img) {
  FILE *f = fopen(path, "wb"); fprintf(f, "P6\n256 224\n255\n");
  for (unsigned i = 0; i < 256 * 224; ++i) { unsigned char c[3] = {img[i] >> 16, img[i] >> 8, img[i]}; fwrite(c, 1, 3, f); }
  fclose(f);
}
int main(int argc, char **argv) {
  char e[512]; if (!Mmx3dsLoad(argv[1], e, sizeof e)) { fprintf(stderr, "%s\n", e); return 1; }
  int stage = atoi(argv[2]); unsigned pairs = strtoul(argv[3], NULL, 10); const char *out = argv[4];
  double acc = 0; unsigned n = 0, differ = 0, shown = 0;
  for (unsigned i = 0; i < pairs; ++i) {
    for (unsigned step = 0; step < 2; ++step) {
      ++n; uint16_t pad = 0;
      if (n >= 2000 && g_ram[0xd1] != 2 && n % 120 < 12) pad = 1u << 3;
      if (g_ram[0xd1] == 2 && g_ram[0xd3] == 4) { pad = (n / 300) % 2 ? 1u << 6 : 1u << 7; if (n % 90 < 20) pad |= 1; }
      if (stage >= 0 && g_ram[0xd1] == 2 && g_ram[0xd3] == 0) g_ram[0x1f7a] = (uint8_t)stage;
      if (!Mmx3dsRunLogic(pad) || !Mmx3dsRunRaster(step == 1)) { printf("fail %u\n", n); return 1; }
      acc += MMX3DS_RATE / 60.0988 * Mmx3dsFramePeriods();
      int16_t pcm[2048]; if(Mmx3dsNativeAudioEnabled()){while(Mmx3dsAudioAvailable())Mmx3dsAudioRead(pcm,1024);}else while (acc >= 1) { unsigned c = acc > 1024 ? 1024 : (unsigned)acc; Mmx3dsAudio(pcm, c); acc -= c; }
    }
    Mmx3dsDrawPair(eyes, eyes + 256 * 224, MMX_STEREO_FULL);
    if (g_ram[0xd1] == 2 && g_ram[0xd3] == 4) {
      ++shown; unsigned d = 0;
      for (unsigned k = 0; k < 256 * 224; ++k) d += eyes[k] != eyes[256 * 224 + k];
      if (d > 500) ++differ;
      if (shown % 150 == 1) {
        Ppu *p = g_ppu;
        printf("stage=%02x n=%u diffpx=%u windowsel=%06lx tmw=%02x/%02x tm=%02x/%02x mosaic=%02x bgmode=%02x stagescene=%d native=%d\n",
            g_ram[0x1f7a], n, d, (unsigned long)p->windowsel, p->screenWindowed[0], p->screenWindowed[1],
            p->screenEnabled[0], p->screenEnabled[1], p->mosaic, p->bgmode, MmxWidePolicy_IsStageScene(g_ram), Mmx3dsPairIsNative());
        char path[512]; snprintf(path, sizeof path, "%s/s%02d_%05u_L.ppm", out, stage, n); save(path, eyes);
      }
    }
  }
  printf("stage %d: %u gameplay pairs, %u with visible disparity\n", stage, shown, differ);
  return 0;
}
