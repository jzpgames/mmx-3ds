#include <3ds.h>
#include "audio_device.h"
#include "audio_queue.h"
#include "game_core.h"
#include <string.h>
#include <assert.h>
/* Main thread owns the guest DSP. The NDSP callback ONLY copies queued PCM
 * and updates device parameters; it never enters game/APU/DSP emulation.
 * libctru 2.7.1 calls us after ndspiReadChnState releases the channel lock,
 * before ndspiUpdateChn: WaveBufAdd and SetRate are safe at this point. */
enum { BUFFERS=8 };
static MmxAudioQueue fifo;
static ndspWaveBuf waves[BUFFERS];
static bool inflight[BUFFERS], active, running, pressure;
static LightLock lock;
static int16_t *memory;
static MmxAudioStatus status;
static double base_rate;
static unsigned control_divider;

static void audio_callback(void *unused) {
  (void)unused; u64 start=svcGetSystemTick();
  LightLock_Lock(&lock);
  if(!active||status.paused){LightLock_Unlock(&lock);return;}
  unsigned queued=0, count=0;
  u16 seq=ndspChnGetWaveBufSeq(0); u32 pos=ndspChnGetSamplePos(0);
  for(unsigned i=0;i<BUFFERS;++i){
    unsigned s=__atomic_load_n(&waves[i].status,__ATOMIC_ACQUIRE);
    if(s==NDSP_WBUF_QUEUED||s==NDSP_WBUF_PLAYING){
      ++count; queued+=waves[i].nsamples;
      if(s==NDSP_WBUF_PLAYING&&waves[i].sequence_id==seq&&pos<=waves[i].nsamples)queued-=pos;
    }else if(inflight[i]){status.completed+=waves[i].nsamples;inflight[i]=false;}
  }
  /* Observe BEFORE replenishing: a genuine device queue exhaustion must not
   * disappear just because the software FIFO happens to contain more data. */
  if(running&&!count){status.min_queued=0;++status.underruns;running=false;status.priming=true;ndspChnSetPaused(0,true);}
  if(running&&queued+fifo.count<status.min_queued)status.min_queued=queued+fifo.count;
  for(unsigned i=0;i<BUFFERS&&fifo.count>=MMXA_PACKET;++i){
    unsigned s=__atomic_load_n(&waves[i].status,__ATOMIC_ACQUIRE);
    if(s!=NDSP_WBUF_FREE&&s!=NDSP_WBUF_DONE)continue;
    bool ok=MmxAudioPop(&fifo,(int16_t*)waves[i].data_vaddr,MMXA_PACKET);assert(ok);
    waves[i].nsamples=MMXA_PACKET;waves[i].looping=false;
    DSP_FlushDataCache(waves[i].data_vaddr,MMXA_PACKET*4);
    ndspChnWaveBufAdd(0,&waves[i]);inflight[i]=true;
    ++count;queued+=MMXA_PACKET;status.submitted+=MMXA_PACKET;
  }
  status.queued=queued+fifo.count;
  if(status.queued>status.max_queued)status.max_queued=status.queued;
  if(!running&&queued>=MMXA_PRIME){running=true;status.priming=false;ndspChnSetPaused(0,false);}
  if(running){
    status.correction=MmxAudioRateStep(&fifo,status.queued);
    if(++control_divider>=20){control_divider=0;status.rate=base_rate*(1+status.correction);ndspChnSetRate(0,(float)status.rate);}
  }
  u64 elapsed=svcGetSystemTick()-start;
  if(elapsed>status.callback_peak_ticks)status.callback_peak_ticks=elapsed;
  LightLock_Unlock(&lock);
}
bool MmxAudioStart(void) {
  LightLock_Init(&lock);memset(&fifo,0,sizeof fifo);memset(&status,0,sizeof status);
  memset(waves,0,sizeof waves);memset(inflight,0,sizeof inflight);
  status.min_queued=MMXA_CAPACITY+BUFFERS*MMXA_PACKET;status.priming=true;status.polyphase=true;
  base_rate=status.rate=MMX3DS_NATIVE_RATE;
  if(R_FAILED(ndspInit()))return false;
  memory=linearAlloc(BUFFERS*MMXA_PACKET*4);
  if(!memory){ndspExit();return false;}
  ndspChnReset(0);ndspChnSetPaused(0,true);
  ndspSetOutputMode(NDSP_OUTPUT_STEREO);ndspChnSetInterp(0,NDSP_INTERP_POLYPHASE);
  ndspChnSetRate(0,(float)base_rate);ndspChnSetFormat(0,NDSP_FORMAT_STEREO_PCM16);
  float mix[12]={1,1};ndspChnSetMix(0,mix);
  for(unsigned i=0;i<BUFFERS;++i)waves[i].data_vaddr=memory+i*MMXA_PACKET*2;
  active=true;running=false;pressure=false;control_divider=0;
  ndspSetCallback(audio_callback,NULL);return true;
}
void MmxAudioSetBaseRate(double rate) {
  LightLock_Lock(&lock);base_rate=rate;status.rate=rate*(1+fifo.correction);
  ndspChnSetRate(0,(float)status.rate);LightLock_Unlock(&lock);
}
bool MmxAudioSubmit(unsigned samples) {
  int16_t pcm[MMXA_PACKET*2];
  while(samples){
    unsigned n=samples>MMXA_PACKET?MMXA_PACKET:samples;
    u64 start=svcGetSystemTick();
    for(;;){
      LightLock_Lock(&lock);
      bool space=MmxAudioSpace(&fifo)>=n;
      if(!space&&!pressure){++status.overruns;pressure=true;}
      LightLock_Unlock(&lock);
      if(space)break;
      /* Backpressure is observed, never an overwrite/drop or silent drain.
       * HOME/sleep/exit remain responsive while waiting for the consumer. */
      if(!aptMainLoop())return false;
      svcSleepThread(500000);
    }
    LightLock_Lock(&lock);status.backpressure_ms+=(svcGetSystemTick()-start)*(1000.0/SYSCLOCK_ARM11);pressure=false;LightLock_Unlock(&lock);
    /* Raw source PCM: no software resampling/fades or invented silence. */
    unsigned actual=Mmx3dsAudioRead(pcm,n);assert(actual==n);(void)actual;
    LightLock_Lock(&lock);bool ok=MmxAudioPush(&fifo,pcm,n);assert(ok);status.generated+=n;LightLock_Unlock(&lock);
    samples-=n;
  }
  return true;
}
void MmxAudioPause(bool paused) {
  if(!active)return;
  LightLock_Lock(&lock);status.paused=paused;
  ndspChnSetPaused(0,paused||!running);LightLock_Unlock(&lock);
}
/* Drop everything queued (device and software FIFO) and prime again; used
 * after a savestate load so the old timeline is not heard. Callers pause
 * first, so the callback returns early while the channel is cleared. */
void MmxAudioFlush(void) {
  if(!active)return;
  ndspChnWaveBufClear(0);
  LightLock_Lock(&lock);
  for(unsigned i=0;i<BUFFERS;++i){waves[i].status=NDSP_WBUF_DONE;inflight[i]=false;}
  fifo.head=fifo.count=0;running=false;status.priming=true;status.queued=0;
  ndspChnSetPaused(0,true);
  LightLock_Unlock(&lock);
}
void MmxAudioToggleInterp(void) {
  LightLock_Lock(&lock);status.polyphase=!status.polyphase;
  ndspChnSetInterp(0,status.polyphase?NDSP_INTERP_POLYPHASE:NDSP_INTERP_LINEAR);LightLock_Unlock(&lock);
}
MmxAudioStatus MmxAudioGetStatus(void) {
  LightLock_Lock(&lock);MmxAudioStatus s=status;LightLock_Unlock(&lock);return s;
}
void MmxAudioResetMetrics(void) {
  LightLock_Lock(&lock);
  status.underruns=status.overruns=status.dropped=0;
  status.min_queued=status.max_queued=status.queued;
  status.backpressure_ms=0;status.callback_peak_ticks=0;
  LightLock_Unlock(&lock);
}
void MmxAudioStop(void) {
  LightLock_Lock(&lock);active=false;LightLock_Unlock(&lock);
  ndspSetCallback(NULL,NULL);
  /* Join the NDSP thread before releasing memory it may still be reading. */
  ndspChnWaveBufClear(0);ndspExit();linearFree(memory);memory=NULL;
}
