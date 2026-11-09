#pragma once
#include <stdbool.h>
#include <stdint.h>
/* Stereo sample FRAMES, not int16 elements. Transport only; no resampling,
 * inserted silence, or overwriting of unread PCM. Caller serializes access. */
enum { MMXA_CAPACITY=65536, MMXA_PACKET=1024, MMXA_PRIME=4096 };
typedef struct { int16_t pcm[MMXA_CAPACITY*2]; unsigned head,count; double correction; } MmxAudioQueue;
unsigned MmxAudioSpace(const MmxAudioQueue *q);
bool MmxAudioPush(MmxAudioQueue *q,const int16_t *pcm,unsigned frames);
bool MmxAudioPop(MmxAudioQueue *q,int16_t *pcm,unsigned frames);
/* Called every NDSP sound frame (~4.89 ms). Deadband, 2 s low pass, bounded
 * +/-0.5%. Positive means consume faster. Does not touch emulated PCM. */
double MmxAudioRateStep(MmxAudioQueue *q,unsigned queued_frames);
