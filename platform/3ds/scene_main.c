/* Host diagnostic (revision 16): scripted input + RAM pokes, periodic PNG-able
 * PPM dumps of both eyes and PPU layer usage. Script lines:
 *   <frame> pad <hex>            pad from this frame on
 *   <frame> poke <hex addr> <hex byte>
 *   <frame> shot                  dump eyes at this frame (pair boundary)
 *   <frame> pwd <12 digits>       write password entry bytes $1E60..
 *   <frame> end */
#include "game_core.h"
#include "mmx_renderer.h"
#include "mmx_wide_policy.h"
#include "common_rtl.h"
#include "snes/ppu.h"
#include "common_cpu_infra.h"
#include "snes/snes.h"
#include "snes/apu.h"
#include "snes/dsp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static uint32_t eyes[256 * 224 * 2], ref[256 * 224 * 2];
static unsigned verified, mismatched;
extern unsigned g_ppu_debug_layer_mask;
static void save(const char *path, const uint32_t *l, const uint32_t *r) {
  FILE *f = fopen(path, "wb"); fprintf(f, "P6\n512 224\n255\n");
  for (unsigned y = 0; y < 224; ++y) for (unsigned x = 0; x < 512; ++x) {
    uint32_t c = x < 256 ? l[y * 256 + x] : r[y * 256 + x - 256];
    unsigned char b[3] = {c >> 16, c >> 8, c}; fwrite(b, 1, 3, f); }
  fclose(f);
}
typedef struct { unsigned frame; char op[8]; unsigned a, b; char s[16]; } Cmd;
int main(int argc, char **argv) {
  char e[512]; if (!Mmx3dsLoad(argv[1], e, sizeof e)) { fprintf(stderr, "%s\n", e); return 1; }
  FILE *sf = fopen(argv[2], "r"); const char *out = argv[3];
  static Cmd cmds[65536]; unsigned nc = 0; char line[256];
  while (fgets(line, sizeof line, sf) && nc < 65536) {
    Cmd c = {0}; char a[32] = "", b[32] = "";
    if (sscanf(line, "%u %7s %31s %31s", &c.frame, c.op, a, b) < 2) continue;
    c.a = strtoul(a, NULL, 16); c.b = strtoul(b, NULL, 16); snprintf(c.s, sizeof c.s, "%s", a);
    cmds[nc++] = c;
  }
  if (getenv("NO_BG1_DEPTH")) g_mmx_stereo_bg1_depth = false;
  if (getenv("STOCK")) MmxRendererSetStockLineSkipping(false);
  FILE *audio_out = getenv("AUDIO_OUT") ? fopen(getenv("AUDIO_OUT"), "wb") : NULL;
  uint16_t pad = 0; unsigned ci = 0, end = 1u << 30;
  int keep_addr[8]; uint8_t keep_val[8]; unsigned nkeep = 0;
  for (unsigned n = 1; n < end; ++n) {
    bool shot = false;
    for (; ci < nc && cmds[ci].frame <= n; ++ci) {
      Cmd *c = &cmds[ci];
      if (!strcmp(c->op, "pad")) pad = (uint16_t)c->a;
      else if (!strcmp(c->op, "poke")) g_ram[c->a & 0x1ffff] = (uint8_t)c->b;
      else if (!strcmp(c->op, "shot")) shot = true;
      else if (!strcmp(c->op, "keep") && nkeep < 8) { keep_addr[nkeep] = (int)(c->a & 0x1ffff); keep_val[nkeep++] = (uint8_t)c->b; }
      else if (!strcmp(c->op, "unkeep")) nkeep = 0;
      else if (!strcmp(c->op, "meta")) {
        /* Metatile map (BG1 world blocks under the camera) and candidate tables. */
        printf("ram b8c..ba4:"); for (unsigned a = 0xb8c; a < 0xba4; ++a) printf(" %02x", g_ram[a]); printf("\n");
        unsigned cx = g_ram[0x1e4d] | (g_ram[0x1e4e] << 8), cy = g_ram[0x1e50] | (g_ram[0x1e51] << 8);
        printf("cam %04x %04x  4d=%04x 50=%04x 5e=%04x 62=%04x\n", cx, cy, g_ram[0x1e4d]|g_ram[0x1e4e]<<8, g_ram[0x1e50]|g_ram[0x1e51]<<8, g_ram[0x1e5e]|g_ram[0x1e5f]<<8, g_ram[0x1e62]|g_ram[0x1e63]<<8);
        for (unsigned by = 0; by < 14; ++by) {
          for (unsigned bx = 0; bx < 16; ++bx) {
            unsigned x = cx + bx * 16, y = cy + by * 16 + 8;
            unsigned screen = g_ram[0xe800 + (y >> 8) * 32 + (x >> 8)];
            unsigned addr = (0x2000 + screen * 512 + ((y & 0xf0) << 1) + ((x & 0xf0) >> 3)) & 0xffff;
            unsigned mt = (g_ram[addr] | (g_ram[addr + 1] << 8)) & 0x3ff;
            unsigned ptr = g_ram[0xb92] | (g_ram[0xb93] << 8) | (g_ram[0xb94] << 16);
            printf(" %03x", mt);
          }
          printf("\n");
        }
      }
      else if (!strcmp(c->op, "prio")) {
        /* BG1/BG2 8x8 tile priority map of the current screen (8x8 tiles). */
        Ppu *p = g_ppu;
        printf("coll ptr %02x%02x%02x windowsel=%06lx tmw=%02x tsw=%02x cgwsel=%02x cgadsub=%02x mosaic=%02x\n", g_ram[0xb94], g_ram[0xb93], g_ram[0xb92],
          (unsigned long)p->windowsel, p->screenWindowed[0], p->screenWindowed[1], p->cgwsel, p->cgadsub, p->mosaic);
        for (unsigned layer = 0; layer < 2; ++layer) {
          printf("BG%u priority map (hs=%u vs=%u sc=%02x):\n", layer + 1, p->hScroll[layer], p->vScroll[layer], p->bgXsc[layer]);
          unsigned base = (p->bgXsc[layer] & 0xfc) << 8, wide = p->bgXsc[layer] & 1, high = (p->bgXsc[layer] >> 1) & 1;
          for (unsigned ty = 0; ty < 28; ++ty) {
            for (unsigned tx = 0; tx < 32; ++tx) {
              unsigned x = (p->hScroll[layer] + tx * 8) & 0x3ff, y = (p->vScroll[layer] + ty * 8 + 1) & 0x3ff;
              unsigned mx = (x >> 3) & 63, my = (y >> 3) & 63, adr = base;
              if (mx >= 32 && wide) adr += 0x400; if (my >= 32 && high) adr += wide ? 0x800 : 0x400;
              uint16_t e = p->vram[(adr + (my & 31) * 32 + (mx & 31)) & 0x7fff];
              putchar((e & 1023) == 0 ? '.' : (e & 0x2000) ? '#' : 'o');
            }
            putchar('\n');
          }
        }
      }
      else if (!strcmp(c->op, "pwd")) for (int i = 0; i < 12 && c->s[i]; ++i) g_ram[0x1e60 + i] = (uint8_t)(c->s[i] - '1');
      else if (!strcmp(c->op, "end")) end = n;
      else if (!strcmp(c->op, "save")) { char er[128]; printf("save %u %d\n", n, Mmx3dsSaveState("scene.sav", NULL, er, sizeof er)); }
      else if (!strcmp(c->op, "load")) { char er[128]; printf("load %u %d %s\n", n, Mmx3dsLoadState("scene.sav", er, sizeof er), er); }
      else if (!strcmp(c->op, "dsp")) { Dsp *d = g_snes->apu->dsp; printf("dsp %u:", n);
        for (int i = 0; i < 8; ++i) printf(" [%d s%02x g%03x v%d/%d]", i, d->channel[i].srcn, d->channel[i].gain, d->channel[i].volumeL, d->channel[i].volumeR);
        printf("\n"); }
      else if (!strcmp(c->op, "ram")) { printf("ram %u:", n); for (unsigned a = c->a; a < c->a + 0x100; ++a) printf(" %02x", g_ram[a]); printf("\n"); }
      else if (!strcmp(c->op, "layers")) {
        /* Dump the frame four times, each with one layer removed. */
        size_t cap = RtlSaveSnapshotToMemory(NULL, 0); void *blob = malloc(cap);
        size_t sz = RtlSaveSnapshotToMemory(blob, cap);
        for (unsigned l = 0; l < 5; ++l) {
          g_ppu_debug_layer_mask = l < 4 ? (l == 3 ? 0x10u : 1u << l) : 0;
          for (unsigned k = 0; k < 2; ++k) { Mmx3dsRunLogic(pad); Mmx3dsRunRaster(k == 1); }
          int16_t pcm[2048]; while (Mmx3dsAudioAvailable()) Mmx3dsAudioRead(pcm, 1024);
          Mmx3dsDrawPair(eyes, eyes + 256 * 224, MMX_STEREO_FULL);
          char path[512]; snprintf(path, sizeof path, "%s/f%05u_no%u.ppm", out, n, l); save(path, eyes, eyes + 256 * 224);
          RtlLoadSnapshotFromMemory(blob, sz);
        }
        g_ppu_debug_layer_mask = 0; free(blob);
      }
    }
    if (n >= end) break;
    for (unsigned k = 0; k < nkeep; ++k) g_ram[keep_addr[k]] = keep_val[k];
    bool present = getenv("ALL") ? true : (n % 2) == 0;
    if (!Mmx3dsRunLogic(pad) || !Mmx3dsRunRaster(present)) { printf("fail %u\n", n); return 1; }
    int16_t pcm[2048]; while (Mmx3dsAudioAvailable()) { unsigned got = Mmx3dsAudioRead(pcm, 1024);
      if (audio_out && got) fwrite(pcm, 4, got, audio_out); }
    if (present && (shot || (n % 200) == 0)) {
      unsigned amt = getenv("AMOUNT") ? (unsigned)atoi(getenv("AMOUNT")) : MMX_STEREO_FULL;
      struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
      int reps = getenv("TIME") ? atoi(getenv("TIME")) : 1;
      for (int k = 0; k < reps; ++k) Mmx3dsDrawPair(eyes, eyes + 256 * 224, amt);
      clock_gettime(CLOCK_MONOTONIC, &t1);
      if (getenv("TIME")) printf("TIME n=%u us=%.1f\n", n, ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / 1e3 / reps);
      if (getenv("VERIFY")) {
        Mmx3dsDraw(ref, MMX_STEREO_LEFT, amt);
        Mmx3dsDraw(ref + 256 * 224, MMX_STEREO_RIGHT, amt);
        if (memcmp(ref, eyes, sizeof ref)) { ++mismatched; unsigned bad = 0, first = 0;
          for (unsigned k = 0; k < 256 * 224 * 2; ++k) if (ref[k] != eyes[k]) { if (!bad) first = k; ++bad; }
          printf("VERIFY MISMATCH n=%u pixels=%u first=%u (eye %u x=%u y=%u)\n", n, bad, first, first / (256*224), first % 256, (first % (256*224)) / 256); }
        else ++verified;
      }
      Ppu *p = g_ppu; unsigned d = 0;
      for (unsigned k = 0; k < 256 * 224; ++k) d += eyes[k] != eyes[256 * 224 + k];
      if (getenv("FLICK")) printf("FLICK n=%u merged=%u\n", n, g_mmx_flicker_merged);
      printf("n=%u mode=%02x/%02x/%02x stage=%02x bgmode=%02x tm=%02x ts=%02x diff=%u native=%d stagescene=%d inidisp=%02x\n",
        n, g_ram[0xd1], g_ram[0xd2], g_ram[0xd3], g_ram[0x1f7a], p->bgmode, p->screenEnabled[0], p->screenEnabled[1], d,
        Mmx3dsPairIsNative(), MmxWidePolicy_IsStageScene(g_ram), p->inidisp);
      if (shot) { char path[512]; snprintf(path, sizeof path, "%s/f%05u.ppm", out, n); save(path, eyes, eyes + 256 * 224); }
      const uint32_t *stk = getenv("STOCK") ? MmxRendererStockFrame() : NULL;
      if (shot && stk) { unsigned bad = 0, x0 = 999, x1 = 0, y0 = 999, y1 = 0;
        for (unsigned k = 0; k < 256 * 224; ++k) if ((stk[k] ^ eyes[k]) & 0xffffff) { ++bad; unsigned x = k % 256, y = k / 256;
          if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
        printf("STOCKDIFF n=%u pixels=%u box=%u,%u-%u,%u\n", n, bad, x0, y0, x1, y1);
        char path[512]; snprintf(path, sizeof path, "%s/s%05u.ppm", out, n); save(path, eyes, stk); }
    }
  }
  if (getenv("VERIFY")) printf("VERIFY ok=%u mismatched=%u\n", verified, mismatched);
  return 0;
}
