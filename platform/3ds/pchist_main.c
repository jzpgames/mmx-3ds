/* Host diagnostic: histogram of interpreter PCs during chosen frames. */
#include "game_core.h"
#include "common_rtl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern void (*g_interp_bridge_pc_hook)(uint32_t, int, int);
static unsigned *hist;
static uint32_t prevpc; static unsigned long long edges[64][2]; static int ne;
static uint32_t watch;
static void hook(uint32_t pc, int m, int x) { (void)m; (void)x; hist[pc & 0xffffff]++;
  if (watch && (pc & 0xffffff) == watch) { int i; for (i = 0; i < ne; ++i) if (edges[i][0] == prevpc) break;
    if (i == ne && ne < 64) { edges[ne][0] = prevpc; edges[ne][1] = 0; ++ne; } if (i < 64) edges[i][1]++; }
  prevpc = pc & 0xffffff; }
static uint16_t pad_for(unsigned n) {
  uint16_t pad = 0;
  if (n >= 2000 && n < 2900 && n % 120 < 12) pad = 1u << 3;
  if (n >= 2900) { pad = 1u << 7; if ((n / 8) % 2) pad |= 1u << 1; if (n % 90 < 25) pad |= 1u << 0;
    if (n % 700 > 600) pad = 1u << 6; if (n % 1500 > 1480) pad = 1u << 3; }
  return pad;
}
static int cmp(const void *a, const void *b) { unsigned x = hist[*(const unsigned *)a], y = hist[*(const unsigned *)b]; return x < y ? 1 : x > y ? -1 : 0; }
int main(int argc, char **argv) {
  char e[512]; if (!Mmx3dsLoad(argv[1], e, sizeof e)) return 1;
  unsigned from = atoi(argv[2]), to = argc > 3 ? atoi(argv[3]) : from;
  hist = calloc(1u << 24, sizeof *hist);
  if (getenv("WATCH")) watch = strtoul(getenv("WATCH"), NULL, 16);
  for (unsigned n = 1; n <= to; ++n) {
    g_interp_bridge_pc_hook = n >= from ? hook : NULL;
    if (!Mmx3dsRunLogic(pad_for(n)) || !Mmx3dsRunRaster(n % 2 == 0)) return 2;
    int16_t pcm[2048]; while (Mmx3dsAudioAvailable()) Mmx3dsAudioRead(pcm, 1024);
  }
  static unsigned idx[1u << 16]; unsigned k = 0; unsigned long long total = 0;
  for (unsigned pc = 0; pc < (1u << 24); ++pc) if (hist[pc]) { total += hist[pc]; if (k < (1u << 16)) idx[k++] = pc; }
  qsort(idx, k, sizeof *idx, cmp);
  for (int i = 0; i < ne; ++i) printf("edge %06X -> %06X x%llu\n", (unsigned)edges[i][0], watch, edges[i][1]);
  printf("total interp ops %llu, distinct pcs %u\n", total, k);
  for (unsigned i = 0; i < k && i < 4000; ++i) printf("%06X %u\n", idx[i], hist[idx[i]]);
  return 0;
}
