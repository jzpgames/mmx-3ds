#include "mmx3ds_audio_verify.h"
#include "game_core.h"
#include "mmx_renderer.h"
#include "mmx_wide_policy.h"
#include "common_rtl.h"
#include "cpu_state.h"
#include "snes/ppu.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <sys/stat.h>
static uint32_t eyes[2*256*224],mono[2*256*224],ref[2*256*224];
static uint64_t hash(uint64_t h,const void*data,size_t n){const uint8_t*p=data;while(n--)h=(h^*p++)*1099511628211ull;return h;}
static clockid_t profile_clock = CLOCK_MONOTONIC;
static double now_ms(void){struct timespec t;clock_gettime(profile_clock,&t);return t.tv_sec*1000.0+t.tv_nsec/1e6;}
static unsigned w(const uint8_t*p){return p[0]|p[1]<<8;}
static void ppm(const char*dir,unsigned frame,const char*tag,const uint32_t*im){if(!strcmp(dir,"-"))return;char path[1200];snprintf(path,sizeof path,"%s/%04u-%s.ppm",dir,frame,tag);FILE*f=fopen(path,"wb");assert(f);fprintf(f,"P6\n256 224\n255\n");for(unsigned i=0;i<256*224;++i){uint8_t b[]={im[i]>>16,im[i]>>8,im[i]};assert(fwrite(b,3,1,f)==1);}fclose(f);}
static int cmp_double(const void*a,const void*b){double x=*(const double*)a,y=*(const double*)b;return(x>y)-(x<y);}
int main(int argc,char**argv){
 if(argc!=5){fprintf(stderr,"ROM stage gameplay-steps output-directory\n");return 2;}
 if(getenv("MMX3DS_PROFILE_CPU"))profile_clock=CLOCK_PROCESS_CPUTIME_ID;
 const unsigned stage=strtoul(argv[2],NULL,0),target=strtoul(argv[3],NULL,0);const char*out=argv[4];if(strcmp(out,"-"))mkdir(out,0755);setvbuf(stdout,NULL,_IONBF,0);
 char error[512];if(!Mmx3dsLoad(argv[1],error,sizeof error)){fprintf(stderr,"%s\n",error);return 1;}
 bool entered=false;unsigned play=0,pairs=0,depth_pairs=0,verified=0;uint64_t hmono=1469598103934665603ull,hstate=hmono,hpcm=hmono;double draw=0,flat=0,logic=0,raster=0;double times[5000];uint64_t changed=0;double acc=0;
 for(unsigned n=1;n<=10000&&play<target;){
  double lc=0,pp=0;
  for(unsigned step=0;step<2;++step,++n){
   if(!entered&&g_ram[0xd1]==2&&g_ram[0xd2]==4&&g_ram[0xd3]==0){entered=true;if(stage){g_ram[0x1f7a]=stage;g_ram[0x1f81]=g_ram[0x1f82]=0;g_ram[0xd2]=2;g_ram[0xd3]=g_ram[0xd4]=0;}printf("ENTER stage=%u frame=%u\n",stage,n);}
   uint16_t input=0;if(!entered&&n>=2000&&g_ram[0xd1]!=2&&n%120<12)input=1u<<3;
   if(entered&&g_ram[0xd1]==2&&g_ram[0xd2]==4&&g_ram[0xd3]==4){++play;if(play>90){input=(1u<<7)|((play%20<10)?1u<<1:0);if(play%90<24)input|=1;}}
   assert(Mmx3dsRunLogic(input));lc+=Mmx3dsTiming().logic_ms;assert(Mmx3dsRunRaster(step==1));pp+=Mmx3dsTiming().raster_ms;
   acc+=MMX3DS_RATE/60.0988*Mmx3dsFramePeriods();int16_t pcm[2048];if(Mmx3dsNativeAudioEnabled()){while(Mmx3dsAudioAvailable()){unsigned k=Mmx3dsAudioRead(pcm,1024);hpcm=hash(hpcm,pcm,k*4);}}else while(acc>=1){unsigned k=acc>1024?1024:(unsigned)acc;Mmx3dsAudio(pcm,k);acc-=k;hpcm=hash(hpcm,pcm,k*4);}
   if(n%100==0){hstate=hash(hstate,g_ram,0x20000);hstate=hash(hstate,g_ppu,PPU_SAVESTATE_REGS_SIZE);hstate=hash(hstate,g_ppu->vram,0x10000);hstate=hash(hstate,g_ppu->oam,512);hstate=hash(hstate,g_ppu->cgram,512);uint16_t cpu[]={g_cpu.A,g_cpu.X,g_cpu.Y,g_cpu.S,g_cpu.D,g_cpu.DB,g_cpu.PB,g_cpu.P,g_cpu.m_flag,g_cpu.x_flag,g_cpu.emulation,g_cpu._flag_N,g_cpu._flag_V,g_cpu._flag_Z,g_cpu._flag_C};hstate=hash(hstate,cpu,sizeof cpu);}
  }
  if(!entered||!play||g_ram[0xd2]!=4||g_ram[0xd3]!=4||g_ram[0x1f7a]!=stage||(g_ppu->inidisp&0x8f)!=15)continue;
  assert(pairs<5000);double t=now_ms();assert(Mmx3dsDrawPair(eyes,eyes+256*224,1024));times[pairs]=now_ms()-t;draw+=times[pairs];logic+=lc;raster+=pp;
  t=now_ms();assert(Mmx3dsDrawPair(mono,mono+256*224,0));flat+=now_ms()-t;assert(!memcmp(mono,mono+256*224,256*224*4));hmono=hash(hmono,mono,256*224*4);
  unsigned d=0;for(unsigned i=0;i<256*224;++i)d+=eyes[i]!=eyes[i+256*224];changed+=d;depth_pairs+=d>0;
  if(pairs%16==0){assert(Mmx3dsDraw(ref,MMX_STEREO_LEFT,1024));assert(Mmx3dsDraw(ref+256*224,MMX_STEREO_RIGHT,1024));if(memcmp(eyes,ref,sizeof eyes)){for(unsigned i=0;i<2*256*224;++i)if(eyes[i]!=ref[i]){fprintf(stderr,"DIFFERENCE stage=%u play=%u eye=%u x=%u y=%u pair=%06x ref=%06x\n",stage,play,i/(256*224),i%256,(i/256)%224,eyes[i],ref[i]);break;}return 3;}++verified;}
  ++pairs;
  if(pairs==1||pairs==30||pairs%90==0){printf("SAMPLE stage=%u play=%u different=%u windowsel=%06x tmw=%02x/%02x screens=%02x/%02x cg=%02x/%02x bgmode=%02x xyz=%u,%u draw=%.3f\n",stage,play,d,(unsigned)g_ppu->windowsel,g_ppu->screenWindowed[0],g_ppu->screenWindowed[1],g_ppu->screenEnabled[0],g_ppu->screenEnabled[1],g_ppu->cgadsub,g_ppu->cgwsel,g_ppu->bgmode,w(g_ram+0xbad),w(g_ram+0xbb0),times[pairs-1]);ppm(out,play,"left",eyes);ppm(out,play,"right",eyes+256*224);ppm(out,play,"mono",mono);}
 }
 qsort(times,pairs,sizeof(double),cmp_double);
 printf("RESULT {\"stage\":%u,\"play\":%u,\"pairs\":%u,\"depth_pairs\":%u,\"different_mean\":%.1f,\"verified\":%u,\"draw_ms\":%.6f,\"draw0_ms\":%.6f,\"p95_ms\":%.6f,\"logic_pair_ms\":%.6f,\"raster_pair_ms\":%.6f,\"mono_hash\":\"%016llx\",\"state_hash\":\"%016llx\",\"pcm_hash\":\"%016llx\",\"raw_pcm_hash\":\"%016llx\",\"raw_frames\":%llu,\"apu_hash\":\"%016llx\"}\n",stage,play,pairs,depth_pairs,pairs?(double)changed/pairs:0,verified,pairs?draw/pairs:0,pairs?flat/pairs:0,pairs?times[(pairs-1)*95/100]:0,pairs?logic/pairs:0,pairs?raster/pairs:0,(unsigned long long)hmono,(unsigned long long)hstate,(unsigned long long)hpcm,(unsigned long long)MmxAudioRawTotal().hash,(unsigned long long)MmxAudioRawTotal().frames,(unsigned long long)Mmx3dsAudioStateHash());
 if(Mmx3dsNativeAudioEnabled()&&MMX_AUDIO_VERIFY&&hpcm!=MmxAudioRawTotal().hash)return 3;
 Mmx3dsShutdown();return pairs>100?0:4;
}
