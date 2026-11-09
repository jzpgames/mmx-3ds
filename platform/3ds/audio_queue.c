#include "audio_queue.h"
#include <string.h>
unsigned MmxAudioSpace(const MmxAudioQueue *q) { return MMXA_CAPACITY-q->count; }
bool MmxAudioPush(MmxAudioQueue *q,const int16_t *pcm,unsigned frames) {
  if(frames>MmxAudioSpace(q))return false;
  unsigned tail=(q->head+q->count)%MMXA_CAPACITY, n=MMXA_CAPACITY-tail;
  if(n>frames)n=frames;
  memcpy(q->pcm+tail*2,pcm,n*4);memcpy(q->pcm,pcm+n*2,(frames-n)*4);
  q->count+=frames;return true;
}
bool MmxAudioPop(MmxAudioQueue *q,int16_t *pcm,unsigned frames) {
  if(frames>q->count)return false;
  unsigned n=MMXA_CAPACITY-q->head;if(n>frames)n=frames;
  memcpy(pcm,q->pcm+q->head*2,n*4);memcpy(pcm+n*2,q->pcm,(frames-n)*4);
  q->head=(q->head+frames)%MMXA_CAPACITY;q->count-=frames;return true;
}
double MmxAudioRateStep(MmxAudioQueue *q,unsigned queued_frames) {
  double error=(double)queued_frames-MMXA_PRIME;
  if(error>512)error-=512;else if(error< -512)error+=512;else error=0;
  double target=error*(0.005/4096.0);
  if(target>0.005)target=0.005;if(target< -0.005)target= -0.005;
  q->correction+=(target-q->correction)*0.00244;
  return q->correction;
}
