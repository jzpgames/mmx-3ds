#include "mmx3ds_audio_metrics.h"
MmxAudioCoreMetrics g_mmx_audio_core_metrics;
#include "mmx3ds_profile.h"
#include <string.h>
#ifdef __3DS__
#include <3ds.h>
static uint64_t ticks(void) { return svcGetSystemTick(); }
#define HZ ((double)SYSCLOCK_ARM11)
#else
#include <time.h>
static uint64_t ticks(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec; }
#define HZ 1000000000.0
#endif
bool g_mmx_profile_enabled;
static unsigned current=MMXP_NONE, generation;
static bool begun;
static uint64_t last, frame[MMXP_COUNT], total[MMXP_COUNT], peak[MMXP_COUNT];
static MmxProfileStats stats;
static uint64_t stamp(void) { ++stats.reads; return ticks(); }
static void charge(uint64_t now) { if(current<MMXP_COUNT) { uint64_t n=now-last; frame[current]+=n; total[current]+=n; } last=now; }
MmxProfileScope MmxProfileEnter(unsigned category) {
  ++stats.calls[category];
  if(current==category)return (MmxProfileScope){0};
  MmxProfileScope s={current,generation,true}; charge(stamp());
  current=category; return s;
}
void MmxProfileLeave(MmxProfileScope *s) {
  if(!s->active||s->generation!=generation)return;
  charge(stamp()); current=s->parent;
}
void MmxProfileFrameBegin(void) {
  MmxProfileFrameEnd(); begun=true;
  ++generation; current=MMXP_NONE; memset(frame,0,sizeof frame);
  if(g_mmx_profile_enabled)last=stamp();
}
void MmxProfileFrameEnd(void) {
  if(!g_mmx_profile_enabled||!begun)return;
  begun=false; charge(stamp()); current=MMXP_NONE; ++stats.frames;
  for(unsigned i=0;i<MMXP_COUNT;++i)if(frame[i]>peak[i])peak[i]=frame[i];
}
void MmxProfileEnable(bool enabled) {
  begun=false; g_mmx_profile_enabled=enabled; current=MMXP_NONE; ++generation;
  memset(frame,0,sizeof frame); memset(total,0,sizeof total); memset(peak,0,sizeof peak);
  memset(&stats,0,sizeof stats);
  uint64_t a=ticks(); for(unsigned i=0;i<2048;++i)(void)ticks();
  stats.clock_ns=(ticks()-a)*(1e9/HZ)/2049;
}
MmxProfileStats MmxProfileTake(void) {
  MmxProfileFrameEnd();
  MmxProfileStats s=stats;
  for(unsigned i=0;i<MMXP_COUNT;++i){s.ms[i]=total[i]*(1000.0/HZ);s.peak_ms[i]=peak[i]*(1000.0/HZ);s.ticks+=total[i];}
  memset(total,0,sizeof total);memset(peak,0,sizeof peak);
  memset(stats.calls,0,sizeof stats.calls);stats.frames=0;stats.reads=0;
  return s;
}
