/* Host check: snapshot in memory mid-stage, play 600 frames, restore, replay
 * the same 600 frames; WRAM/VRAM/CGRAM/OAM and PCM must match exactly. */
#include "game_core.h"
#include "common_rtl.h"
#include "snes/ppu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
static uint64_t fnv(const void *p, size_t n, uint64_t h) {
  const uint8_t *b = p; for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; } return h;
}
static uint16_t pad_for(unsigned n) {
  uint16_t pad = 0;
  if (n >= 2000 && n < 2900 && n % 120 < 12) pad = 1u << 3;
  if (n >= 2900) { pad = 1u << 7; if ((n / 8) % 2) pad |= 1u << 1; if (n % 90 < 25) pad |= 1u << 0; }
  return pad;
}
static double acc;
static int16_t *pcmlog; static size_t pcmlen; static int logging;
static uint64_t parts[4];
static uint64_t step(unsigned n) {
  if (!Mmx3dsRunLogic(pad_for(n)) || !Mmx3dsRunRaster(n % 2 == 0)) { printf("FAIL %u\n", n); exit(1); }
  acc += MMX3DS_RATE / 60.0988 * Mmx3dsFramePeriods();
  int16_t pcm[2048]; uint64_t h = 1469598103934665603ull;
  while (acc >= 1) { unsigned c = acc > 1024 ? 1024 : (unsigned)acc; Mmx3dsAudio(pcm, c); h = fnv(pcm, c * 4, h); if (logging) { memcpy(pcmlog + pcmlen, pcm, c * 4); pcmlen += c * 2; } acc -= c; }
  parts[0] = h; parts[1] = fnv(g_ram, 0x20000, 1); parts[2] = fnv(g_ppu->vram, sizeof g_ppu->vram, 1);
  parts[3] = fnv(g_ppu->cgram, sizeof g_ppu->cgram, fnv(g_ppu->oam, sizeof g_ppu->oam, 1));
  return parts[0] ^ parts[1] * 3 ^ parts[2] * 5 ^ parts[3] * 7;
}
int main(int argc, char **argv) {
  char e[512]; if (!Mmx3dsLoad(argv[1], e, sizeof e)) { fprintf(stderr, "%s\n", e); return 1; }
  unsigned at = argc > 2 ? atoi(argv[2]) : 3300, len = 600, n = 1;
  for (; n < at; ++n) step(n);
  size_t size = 1; char err[128];
  const char *spath = argc > 3 ? argv[3] : "/tmp/mmx-sstest.sav";
  if (!Mmx3dsSaveState(spath, NULL, err, sizeof err)) { printf("save failed: %s\n", err); return 1; }
  { Mmx3dsStateInfo inf; printf("info ok=%d stage=%02x\n", Mmx3dsReadStateInfo(spath, &inf), inf.stage); }
  double acc0 = acc;
  printf("snapshot at frame %u: %zu (mode %02x/%02x/%02x)\n", at, size, g_ram[0xd1], g_ram[0xd2], g_ram[0xd3]);
  if (!size) return 1;
  pcmlog = malloc(64u << 20); static int16_t *runA; size_t lenA;
  static uint64_t first[600], fp[600][4];
  logging = 1; pcmlen = 0;
  for (unsigned i = 0; i < len; ++i) { first[i] = step(at + i); memcpy(fp[i], parts, sizeof parts); }
  runA = malloc(pcmlen * 2); memcpy(runA, pcmlog, pcmlen * 2); lenA = pcmlen; pcmlen = 0;
  if (!Mmx3dsLoadState(spath, err, sizeof err)) { printf("load failed: %s\n", err); return 1; }
  acc = acc0; unsigned diff = 0, firstdiff = 0;
  unsigned pd[4] = {0};
  for (unsigned i = 0; i < len; ++i) { if (step(at + i) != first[i]) { if (!diff) firstdiff = i; ++diff; }
    for (int k = 0; k < 4; ++k) pd[k] += parts[k] != fp[i][k]; }
  { long best = -99999; size_t bestm = 0;
    for (long off = -4096; off <= 4096; off += 2) { size_t m = 0, t = 0;
      for (size_t i = 8192; i + 8192 < lenA && t < 200000; ++i, ++t) { long j = (long)i + off; if (j >= 0 && (size_t)j < pcmlen && runA[i] == pcmlog[j]) ++m; }
      if (m > bestm) { bestm = m; best = off; } }
    printf("audio: best alignment offset %ld samples (stereo pairs %ld), match %.2f%%\n", best, best / 2, 100.0 * bestm / 200000);
    size_t eq = 0, n = lenA < pcmlen ? lenA : pcmlen; for (size_t i = 0; i < n; ++i) eq += runA[i] == pcmlog[i];
    { double num = 0, da = 0, db = 0; size_t blocks = n / 2048, close = 0;
      for (size_t b = 0; b < blocks; ++b) { double ea = 0, eb = 0;
        for (size_t i = b * 2048; i < (b + 1) * 2048; ++i) { ea += (double)runA[i] * runA[i]; eb += (double)pcmlog[i] * pcmlog[i]; }
        ea = sqrt(ea / 2048); eb = sqrt(eb / 2048); num += ea * eb; da += ea * ea; db += eb * eb;
        if (fabs(ea - eb) <= 0.15 * (ea > eb ? ea : eb) + 50) ++close; }
      printf("audio loudness envelope correlation %.3f, blocks within 15%%: %zu/%zu\n", num / sqrt(da * db), close, blocks); }
    printf("audio unshifted equal: %.2f%%, lengths %zu vs %zu\n", 100.0 * eq / n, lenA, pcmlen); }
  printf("differing frames by part: audio %u, wram %u, vram %u, cgram+oam %u\n", pd[0], pd[1], pd[2], pd[3]);
  printf("replay after load: %u/%u frames differ%s", diff, len, diff ? "" : " (identical)\n");
  if (diff) printf(", first at +%u\n", firstdiff);
  return diff != 0;
}
