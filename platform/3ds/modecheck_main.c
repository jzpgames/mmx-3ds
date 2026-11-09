#include "mmx3ds_audio_verify.h"
/* Same fixed input timeline; prints a hash of WRAM+VRAM+CGRAM+OAM per frame,
 * so two scheduler modes can be compared frame by frame. Host only. */
#include "game_core.h"
#include "common_rtl.h"
#include "snes/ppu.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec * 1e-6; }
#if __has_include(<valgrind/callgrind.h>)
#include <valgrind/callgrind.h>
#else
#define CALLGRIND_START_INSTRUMENTATION
#define CALLGRIND_STOP_INSTRUMENTATION
#endif
static uint64_t fnv(const void *p, size_t n, uint64_t h) {
  const uint8_t *b = p; for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; } return h;
}
int main(int argc, char **argv) {
  char e[512]; if (!Mmx3dsLoad(argv[1], e, sizeof e)) { fprintf(stderr, "%s\n", e); return 1; }
  unsigned frames = strtoul(argv[2], NULL, 10); double acc = 0;
  for (unsigned n = 1; n <= frames; ++n) {
    { static int from=-1, to=-1; if (from<0) { from=getenv("CGFROM")?atoi(getenv("CGFROM")):3000; to=getenv("CGTO")?atoi(getenv("CGTO")):3600; }
      if ((int)n == from) CALLGRIND_START_INSTRUMENTATION;
      if ((int)n == to && getenv("CGSTOP")) { CALLGRIND_STOP_INSTRUMENTATION; return 0; } }
    uint16_t pad = 0;
    if (n >= 2000 && n < 2900 && n % 120 < 12) pad = 1u << 3;
    if (n >= 2900) { pad = 1u << 7; if ((n / 8) % 2) pad |= 1u << 1; if (n % 90 < 25) pad |= 1u << 0;
      if (n % 700 > 600) pad = 1u << 6; if (n % 1500 > 1480) pad = 1u << 3; }
    MmxAudioRawBegin();
    double t0 = now_ms();
    if (!Mmx3dsRunLogic(pad) || !Mmx3dsRunRaster(n % 2 == 0)) { printf("FAIL %u\n", n); return 1; }
    if (getenv("TIMES")) { printf("T %u %.3f %02x/%02x/%02x x=%u objs=%u\n", n, now_ms() - t0, g_ram[0xd1], g_ram[0xd2], g_ram[0xd3], g_ram[0xbad]|g_ram[0xbae]<<8, 0); }
    acc += MMX3DS_RATE / 60.0988 * Mmx3dsFramePeriods();
    int16_t pcm[2048]; uint64_t ah = 1469598103934665603ull;
    unsigned delivered=0;
    if(Mmx3dsNativeAudioEnabled()){while(Mmx3dsAudioAvailable()){unsigned k=Mmx3dsAudioRead(pcm,1024);ah=fnv(pcm,k*4,ah);delivered+=k;}}
    else while (acc >= 1) { unsigned c = acc > 1024 ? 1024 : (unsigned)acc; Mmx3dsAudio(pcm, c); ah = fnv(pcm, c * 4, ah); acc -= c; }
    if (getenv("NOHASH")) continue;
    uint64_t h = fnv(g_ram, 0x20000, 1469598103934665603ull);
    h = fnv(g_ppu->vram, sizeof g_ppu->vram, h); h = fnv(g_ppu->cgram, sizeof g_ppu->cgram, h); h = fnv(g_ppu->oam, sizeof g_ppu->oam, h);
    if (getenv("XY")) { printf("%u %02x/%02x/%02x x=%u y=%u cam=%u,%u hp=%u objs=%016llx\n", n, g_ram[0xd1], g_ram[0xd2], g_ram[0xd3], g_ram[0xbad]|g_ram[0xbae]<<8, g_ram[0xbb0]|g_ram[0xbb1]<<8, g_ram[0x1e4d]|g_ram[0x1e4e]<<8, g_ram[0x1e50]|g_ram[0x1e51]<<8, g_ram[0xbcf], (unsigned long long)fnv(g_ram+0xe68, 0x1228-0xe68+64, 1469598103934665603ull)); continue; }
    MmxAudioRawVerify raw=MmxAudioRawTake();
    if(Mmx3dsNativeAudioEnabled()&&MMX_AUDIO_VERIFY&&(raw.frames!=delivered||raw.hash!=ah)){fprintf(stderr,"RAW PCM MISMATCH %u\n",n);return 3;}
    printf("%u %016llx %016llx %02x/%02x/%02x %.2f %016llx %llu %016llx %u\n",n,(unsigned long long)h,(unsigned long long)ah,g_ram[0xd1],g_ram[0xd2],g_ram[0xd3],Mmx3dsFramePeriods(),(unsigned long long)raw.hash,(unsigned long long)raw.frames,(unsigned long long)Mmx3dsAudioStateHash(),delivered);
  }
  return 0;
}
