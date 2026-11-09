/* The very same core as the 3DS executable, tested without SDL or a window. */
#include "game_core.h"
#include "common_rtl.h"
#include "snes/ppu.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

static uint32_t image[256 * 224];
static void save_image(const char *dir, unsigned frame, const char *eye) {
  char path[1024]; snprintf(path, sizeof(path), "%s/%06u-%s.ppm", dir, frame, eye);
  FILE *f = fopen(path, "wb"); if (!f) { perror(path); exit(1); }
  fprintf(f, "P6\n256 224\n255\n");
  for (unsigned i = 0; i < 256 * 224; ++i) {
    unsigned char rgb[3] = {image[i] >> 16, image[i] >> 8, image[i]};
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
}
int main(int argc, char **argv) {
  if (argc < 4) { fprintf(stderr, "usage: %s ROM frames output-directory\n", argv[0]); return 2; }
  char error[512];
  if (!Mmx3dsLoad(argv[1], error, sizeof(error))) { fprintf(stderr, "%s\n", error); return 1; }
  unsigned frames = (unsigned)strtoul(argv[2], NULL, 10); mkdir(argv[3], 0777);
  uint64_t nonzero_pcm = 0; double acc = 0;
  for (unsigned n = 1; n <= frames; ++n) {
    /* Optional test script: repeated Start pulses skip the title and select X. */
    uint16_t pad = 0;
    if (getenv("MMX3DS_AUTOSTART") && n >= 2000 && g_ram[0xd1] != 2 && n % 120 < 12)
      pad = 1u << 3;
    if (getenv("MMX3DS_WALK") && g_ram[0xd1] == 2 && g_ram[0xd3] == 4)
      pad = (1u << 7) | (1u << 1);
    if (!Mmx3dsStep(pad)) { fprintf(stderr, "Frame failed: %u\n", n); return 1; }
    acc += MMX3DS_RATE / 60.0988 * Mmx3dsFramePeriods();
    int16_t pcm[1024 * 2];
    if(Mmx3dsNativeAudioEnabled()){while(Mmx3dsAudioAvailable()){unsigned count=Mmx3dsAudioRead(pcm,1024);for(unsigned i=0;i<count*2;++i)nonzero_pcm+=pcm[i]!=0;}}
    else while (acc >= 1) {
      unsigned count = acc > 1024 ? 1024 : (unsigned)acc;
      Mmx3dsAudio(pcm, count); acc -= count;
      for (unsigned i = 0; i < count * 2; ++i) nonzero_pcm += pcm[i] != 0;
    }
    if (n % 100 == 0 || n == frames) {
      Mmx3dsDraw(image, MMX_STEREO_MONO, 0); save_image(argv[3], n, "mono");
      Mmx3dsDraw(image, MMX_STEREO_LEFT, MMX_STEREO_FULL); save_image(argv[3], n, "left");
      Mmx3dsDraw(image, MMX_STEREO_RIGHT, MMX_STEREO_FULL); save_image(argv[3], n, "right");
      fprintf(stderr, "frame=%u mode=%02x/%02x/%02x stage=%02x ppu=%02x pcm_nonzero=%llu xy=%u,%u\n",
              n, g_ram[0xd1], g_ram[0xd2], g_ram[0xd3], g_ram[0x1f7a], g_ppu->inidisp,
              (unsigned long long)nonzero_pcm, g_ram[0xbad] | g_ram[0xbae] << 8,
              g_ram[0xbb0] | g_ram[0xbb1] << 8);
    }
  }
  Mmx3dsShutdown(); return 0;
}
