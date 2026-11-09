#pragma once
#include <stdint.h>
#include <stdbool.h>
/* Main-thread-only storage outside guest state. Only catch-up bursts grow it;
 * the ordinary 8192-frame DSP ring and its hot working set stay unchanged. */
typedef struct {int16_t *pcm;unsigned capacity,head,count,peak;} MmxNativeQueue;
bool MmxNativeReserve(MmxNativeQueue*,unsigned);
bool MmxNativePush(MmxNativeQueue*,const int16_t*,unsigned);
unsigned MmxNativePop(MmxNativeQueue*,int16_t*,unsigned);
void MmxNativeFree(MmxNativeQueue*);
