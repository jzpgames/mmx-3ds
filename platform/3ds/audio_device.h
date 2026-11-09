#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct {
  unsigned underruns, overruns, queued, min_queued, max_queued, dropped;
  uint64_t generated, submitted, completed, callback_peak_ticks;
  double rate, correction, backpressure_ms;
  bool priming, paused, polyphase;
} MmxAudioStatus;
bool MmxAudioStart(void);
void MmxAudioSetBaseRate(double rate);
bool MmxAudioSubmit(unsigned samples);
void MmxAudioPause(bool paused);
void MmxAudioFlush(void);
void MmxAudioToggleInterp(void);
MmxAudioStatus MmxAudioGetStatus(void);
void MmxAudioResetMetrics(void);
void MmxAudioStop(void);
