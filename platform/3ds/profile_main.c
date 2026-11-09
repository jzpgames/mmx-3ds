#include "mmx3ds_audio_verify.h"
#include "mmx3ds_audio_metrics.h"
#include "mmx3ds_profile.h"
/* Host profile of the 3DS frame loop: two guest frames per stereo pair,
 * only the second captured, as on the console. Reports per-pair costs and
 * which compositor path each pair used. Host timings only. */
#include "game_core.h"
#include "mmx_renderer.h"
#include "common_rtl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if __has_include(<valgrind/callgrind.h>)
#include <valgrind/callgrind.h>
#else
#define CALLGRIND_START_INSTRUMENTATION
#define CALLGRIND_STOP_INSTRUMENTATION
#endif
static uint32_t eyes[256 * 224 * 2], ref[256 * 224 * 2];
static unsigned verified, mismatched;
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec * 1e-6; }
int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: %s ROM pairs [csv]\n", argv[0]); return 2; }
  char error[512];
  if (!Mmx3dsLoad(argv[1], error, sizeof(error))) { fprintf(stderr, "%s\n", error); return 1; }
  unsigned pairs = strtoul(argv[2], NULL, 10);
  if (getenv("NO_BG1_DEPTH")) g_mmx_stereo_bg1_depth = false;
  FILE *csv = argc > 3 ? fopen(argv[3], "w") : NULL;
  if (csv) fprintf(csv, "pair,frame,mode,stage,logic_ms,raster_ms,draw_ms,native,reason,custom_lines,game_ms,bridge_ms,spc_ms,dsp_ms,ppu_exclusive_ms,pcm_ms,clock_reads,dsp_drops,render_underruns,priming_frames\n");
  double acc = 0; unsigned n = 0;
  for (unsigned i = 0; i < pairs; ++i) {
    double logic = 0, raster = 0;
    for (unsigned step = 0; step < 2; ++step) {
      ++n;
      uint16_t pad = 0;
      if (n >= 2000 && g_ram[0xd1] != 2 && n % 120 < 12) pad = 1u << 3;
      if (g_ram[0xd1] == 2 && g_ram[0xd3] != 4 && getenv("PAUSE") && n % 600 >= 300 && n % 600 < 304) pad = 1u << 3;
      if (g_ram[0xd1] == 2 && g_ram[0xd3] == 4) {
        pad = (1u << 7) | ((n / 8) % 2 ? (1u << 1) : 0);   /* right, fire */
        if (n % 90 < 25) pad |= 1u << 0;                     /* jump */
        if (n % 400 < 120) pad |= 1u << 8;                   /* dash */
        if (getenv("PAUSE") && n % 600 >= 300 && n % 600 < 304) pad = 1u << 3; /* START */
      }
      MmxAudioRawBegin();
      if (!Mmx3dsRunLogic(pad)) { fprintf(stderr, "logic failed %u\n", n); return 1; }
      logic += Mmx3dsTiming().logic_ms;
      if (!Mmx3dsRunRaster(step == 1)) { fprintf(stderr, "raster failed %u\n", n); return 1; }
      raster += Mmx3dsTiming().raster_ms;
      acc += MMX3DS_RATE / 60.0988 * Mmx3dsFramePeriods();
      int16_t pcm[1024 * 2];
      uint64_t oh=1469598103934665603ull;unsigned delivered=0;
      if(Mmx3dsNativeAudioEnabled()){
       while(Mmx3dsAudioAvailable()){unsigned k=Mmx3dsAudioRead(pcm,1024);const uint8_t*b=(const uint8_t*)pcm;for(unsigned j=0;j<k*4;++j)oh=(oh^b[j])*1099511628211ull;delivered+=k;}
       MmxAudioRawVerify raw=MmxAudioRawTake();if(MMX_AUDIO_VERIFY&&(raw.hash!=oh||raw.frames!=delivered)){fprintf(stderr,"RAW mismatch %u\n",n);return 3;}
      }else while (acc >= 1) { unsigned c = acc > 1024 ? 1024 : (unsigned)acc; Mmx3dsAudio(pcm, c); acc -= c; }
    }
    bool native = Mmx3dsPairIsNative();
    unsigned reason = MmxRendererStereoPairReason();
    double t = now_ms();
    bool cg = getenv("CGDRAW") && g_ram[0xd1] == 2 && g_ram[0xd3] == 4;
    if (cg) CALLGRIND_START_INSTRUMENTATION;
    if (!Mmx3dsDrawPair(eyes, eyes + 256 * 224, MMX_STEREO_FULL)) { fprintf(stderr, "draw failed\n"); return 1; }
    double draw = now_ms() - t;
    if (cg) CALLGRIND_STOP_INSTRUMENTATION;
    if (getenv("VERIFY")) {
      Mmx3dsDraw(ref, MMX_STEREO_LEFT, MMX_STEREO_FULL);
      Mmx3dsDraw(ref + 256 * 224, MMX_STEREO_RIGHT, MMX_STEREO_FULL);
      ++verified; if (memcmp(ref, eyes, sizeof(ref))) { ++mismatched; fprintf(stderr, "MISMATCH pair %u frame %u\n", i, n); }
    }
    MmxRenderStats st = MmxRendererGetStats();
    MmxProfileStats subs=MmxProfileTake();
    if (csv) fprintf(csv, "%u,%u,%02x/%02x/%02x,%02x,%.3f,%.3f,%.3f,%d,%u,%u,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%llu,%llu,%llu,%llu\n", i, n, g_ram[0xd1], g_ram[0xd2], g_ram[0xd3],
        g_ram[0x1f7a], logic, raster, draw, native, reason, st.custom_lines,subs.ms[0],subs.ms[1],subs.ms[2],subs.ms[3],subs.ms[4],subs.ms[5],(unsigned long long)subs.reads,(unsigned long long)g_mmx_audio_core_metrics.dsp_dropped,(unsigned long long)g_mmx_audio_core_metrics.render_underruns,(unsigned long long)g_mmx_audio_core_metrics.priming_frames,(unsigned long long)g_mmx_audio_core_metrics.spill_events,(unsigned long long)g_mmx_audio_core_metrics.spill_frames,(unsigned long long)g_mmx_audio_core_metrics.backlog_peak,(unsigned long long)g_mmx_audio_core_metrics.native_delivered,(unsigned long long)MmxAudioRawTotal().frames,(unsigned long long)MmxAudioRawTotal().hash);
    if (getenv("TRACE") && (i % 15 == 0)) fprintf(stderr, "pair %u mode %02x/%02x/%02x/%02x native %d reason %x draw %.2f c3=%02x 1f10=%02x\n", i, g_ram[0xd1], g_ram[0xd2], g_ram[0xd3], g_ram[0xd4], native, reason, draw, g_ram[0xc3], g_ram[0x1f10]);
  }
  if (csv) fclose(csv);
  if (verified) printf("verified %u real pairs against the reference: %u mismatches\n", verified, mismatched);
  fprintf(stderr,"CORE_AUDIO ring_drops=%llu render_underruns=%llu missing_frames=%llu priming_frames=%llu spill_events=%llu spill_frames=%llu backlog_peak=%llu native_delivered=%llu raw_frames=%llu raw_hash=%016llx\n",(unsigned long long)g_mmx_audio_core_metrics.dsp_dropped,(unsigned long long)g_mmx_audio_core_metrics.render_underruns,(unsigned long long)g_mmx_audio_core_metrics.render_missing,(unsigned long long)g_mmx_audio_core_metrics.priming_frames,(unsigned long long)g_mmx_audio_core_metrics.spill_events,(unsigned long long)g_mmx_audio_core_metrics.spill_frames,(unsigned long long)g_mmx_audio_core_metrics.backlog_peak,(unsigned long long)g_mmx_audio_core_metrics.native_delivered,(unsigned long long)MmxAudioRawTotal().frames,(unsigned long long)MmxAudioRawTotal().hash);
  Mmx3dsShutdown(); return mismatched ? 3 : 0;
}
