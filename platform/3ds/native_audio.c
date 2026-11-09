#include "native_audio.h"
#include "mmx3ds_audio_verify.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>
bool MmxNativeReserve(MmxNativeQueue*q,unsigned frames){
 if(frames<=q->capacity)return true;
 unsigned cap=q->capacity?q->capacity:8192;
 while(cap<frames){if(cap>UINT_MAX/8)return false;cap*=2;}
 if(cap>SIZE_MAX/sizeof(int16_t)/2)return false;
 int16_t*d=malloc((size_t)cap*4);if(!d)return false;
 unsigned n=q->capacity-q->head;if(n>q->count)n=q->count;
 if(n)memcpy(d,q->pcm+q->head*2,n*4);
 if(q->count>n)memcpy(d+n*2,q->pcm,(q->count-n)*4);
 free(q->pcm);q->pcm=d;q->capacity=cap;q->head=0;return true;
}
bool MmxNativePush(MmxNativeQueue*q,const int16_t*p,unsigned frames){
 if(frames>UINT_MAX-q->count||!MmxNativeReserve(q,q->count+frames))return false;
 if(!frames)return true;
 unsigned tail=(q->head+q->count)%q->capacity,n=q->capacity-tail;if(n>frames)n=frames;
 memcpy(q->pcm+tail*2,p,n*4);memcpy(q->pcm,p+n*2,(frames-n)*4);
 q->count+=frames;if(q->count>q->peak)q->peak=q->count;return true;
}
unsigned MmxNativePop(MmxNativeQueue*q,int16_t*p,unsigned capacity){
 unsigned frames=capacity<q->count?capacity:q->count;if(!frames)return 0;
 unsigned n=q->capacity-q->head;if(n>frames)n=frames;
 memcpy(p,q->pcm+q->head*2,n*4);memcpy(p+n*2,q->pcm,(frames-n)*4);
 q->head=(q->head+frames)%q->capacity;q->count-=frames;return frames;
}
void MmxNativeFree(MmxNativeQueue*q){free(q->pcm);memset(q,0,sizeof *q);}
static MmxAudioRawVerify frame,total;
void MmxAudioRawBegin(void){frame=(MmxAudioRawVerify){1469598103934665603ull,0};}
void MmxAudioRawReset(void){MmxAudioRawBegin();total=frame;}
MmxAudioRawVerify MmxAudioRawTake(void){return frame;}
MmxAudioRawVerify MmxAudioRawTotal(void){return total;}
#if MMX_AUDIO_VERIFY
void MmxAudioRawObserve(int16_t left,int16_t right){
 uint16_t a=(uint16_t)left,b=(uint16_t)right;const uint8_t bytes[4]={a,a>>8,b,b>>8};
 for(unsigned i=0;i<4;++i){frame.hash=(frame.hash^bytes[i])*1099511628211ull;total.hash=(total.hash^bytes[i])*1099511628211ull;}
 ++frame.frames;++total.frames;
}
#endif
