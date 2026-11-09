/* Reference and paired compositor on the SAME real-game snapshots.
 * Timings measure this host only; they do not predict physical 3DS FPS. */
#include "game_core.h"
#include "mmx_renderer.h"
#include "common_rtl.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static uint32_t expected[256 * 224 * 2], actual[256 * 224 * 2];
static double now_ms(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000.0 + t.tv_nsec * 1e-6;
}

int main(int argc, char **argv) {
  if (argc != 2) { fprintf(stderr, "Usage: %s ROM\n", argv[0]); return 2; }
  char error[512];
  if (!Mmx3dsLoad(argv[1], error, sizeof(error))) { fprintf(stderr, "%s\n", error); return 1; }
  double audio_fraction = 0, reference_ms = 0, paired_ms = 0;
  unsigned count = 0, fast_lines = 0;
  for (unsigned n = 1; n <= 3300; ++n) {
    uint16_t input = 0;
    if (n >= 2000 && g_ram[0xd1] != 2 && n % 120 < 12) input = 1u << 3;
    if (g_ram[0xd1] == 2 && g_ram[0xd3] == 4) input = (1u << 7) | (1u << 1);
    assert(Mmx3dsStep(input));
    audio_fraction += MMX3DS_RATE / 60.0988 * Mmx3dsFramePeriods();
    int16_t pcm[1024 * 2];
    if(Mmx3dsNativeAudioEnabled()){while(Mmx3dsAudioAvailable())Mmx3dsAudioRead(pcm,1024);}
    else while (audio_fraction >= 1) {
      unsigned samples = audio_fraction > 1024 ? 1024 : (unsigned)audio_fraction;
      Mmx3dsAudio(pcm, samples); audio_fraction -= samples;
    }
    if (n < 2900) continue;
    double start = now_ms();
    assert(Mmx3dsDraw(expected, MMX_STEREO_LEFT, MMX_STEREO_FULL));
    assert(Mmx3dsDraw(expected + 256 * 224, MMX_STEREO_RIGHT, MMX_STEREO_FULL));
    reference_ms += now_ms() - start;
    start = now_ms();
    assert(Mmx3dsDrawPair(actual, actual + 256 * 224, MMX_STEREO_FULL));
    paired_ms += now_ms() - start;
    if (memcmp(expected, actual, sizeof(expected))) {
      fprintf(stderr, "Different eyes at frame %u\n", n); return 1;
    }
    fast_lines += MmxRendererFastStereoLines(); ++count;
  }
  printf("Real ROM: %u stereo frames identical, %u fast lines.\n", count, fast_lines);
  printf("Reference %.3f ms/pair; optimized %.3f ms/pair; ratio %.2fx on HOST ONLY.\n",
      reference_ms / count, paired_ms / count, reference_ms / paired_ms);
  Mmx3dsShutdown(); return 0;
}
