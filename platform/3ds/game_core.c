#include "native_audio.h"
#include "msu3ds.h"
#include "mmx3ds_audio_metrics.h"
#include "mmx3ds_audio_verify.h"
#include "snes/apu.h"
#include "snes/spc.h"
#include "snes/dsp.h"
#include <stddef.h>
#include "mmx3ds_profile.h"
#include "game_core.h"
static bool english; /* 1.0.2: language of the messages below */
void Mmx3dsSetEnglish(bool on) { english = on; }
#define TR(es, en) (english ? (en) : (es))
#include "common_cpu_infra.h"
#include "common_rtl.h"
#include "cpu_state.h"
#include "mmx_rtl.h"
#include "mmx_renderer.h"
#include "mmx_render_assets.h"
#include "mmx_coop.h"
#include "mmx_coop_view.h"
#include "mmx_zero.h"
#include "mmx_weapons.h"
#include "mmx_boss_rush.h"
#include "sha256.h"
#include "snes/snes.h"
#include "snes/cart.h"
#include "snes/ppu.h"
#include "snes/interp_bridge.h"
#include "ppu_dma_trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __3DS__
#include <3ds.h>
#else
#include <time.h>
#endif

static uint32_t stock[MMX3DS_WIDTH * MMX3DS_HEIGHT];
static bool loaded, captured;
static bool native_audio;
static MmxNativeQueue native_backlog;
static void spill_audio(Dsp*dsp){
 unsigned n=dsp_available(dsp),idx=dsp->sampleRead&(DSP_SAMPLE_RING-1);
 if(n>UINT32_MAX-native_backlog.count||!MmxNativeReserve(&native_backlog,native_backlog.count+n)){
  fprintf(stderr,"MMX3DS: cannot preserve PCM burst: out of memory\n");abort();
 }
 unsigned first=DSP_SAMPLE_RING-idx;if(first>n)first=n;
 if(!MmxNativePush(&native_backlog,dsp->sampleBuffer+idx*2,first)||!MmxNativePush(&native_backlog,dsp->sampleBuffer,n-first))abort();
 dsp_advance(dsp,n);++g_mmx_audio_core_metrics.spill_events;g_mmx_audio_core_metrics.spill_frames+=n;
 if(native_backlog.count>g_mmx_audio_core_metrics.backlog_peak)g_mmx_audio_core_metrics.backlog_peak=native_backlog.count;
}
bool Mmx3dsNativeAudioEnabled(void){return native_audio;}
unsigned Mmx3dsAudioAvailable(void){return loaded?native_backlog.count+dsp_available(g_snes->apu->dsp):0;}
unsigned Mmx3dsAudioRead(int16_t*pcm,unsigned capacity){
 MMX_PROFILE_SCOPE(MMXP_AUDIO);
 if(!loaded||!native_audio||!capacity)return 0;
 unsigned n=MmxNativePop(&native_backlog,pcm,capacity);
 n+=dsp_getSamples(g_snes->apu->dsp,pcm+n*2,(int)(capacity-n));
 Mmx3dsMsuMix(pcm,n); /* revision 18: MSU-1 music over the SPC effects */
 g_mmx_audio_core_metrics.native_delivered+=n;return n;
}
static uint64_t audio_hash(const void*p,size_t n,uint64_t h){const uint8_t*b=p;while(n--)h=(h^*b++)*1099511628211ull;return h;}
uint64_t Mmx3dsAudioStateHash(void){
 Apu*a=g_snes->apu;Dsp*d=a->dsp;
 uint64_t h=audio_hash(a->ram,sizeof(Apu)-offsetof(Apu,ram),1469598103934665603ull);
 h=audio_hash(&a->spc->a,sizeof(Spc)-offsetof(Spc,a),h);
 return audio_hash(d->ram,offsetof(Dsp,sampleBuffer)-offsetof(Dsp,ram),h);
}

static Mmx3dsFrameTiming timing;
static double now_ms(void) {
#ifdef __3DS__
  return svcGetSystemTick() * (1000.0 / SYSCLOCK_ARM11);
#else
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000.0 + t.tv_nsec * 1e-6;
#endif
}
Mmx3dsFrameTiming Mmx3dsTiming(void) { return timing; }
static const MmxRenderView view = {256, 0, 256.0 / 224.0};
static const RtlGameInfo info = {
  .title = "mmx-3ds",
  .run_frame = RunOneFrameOfGame,
  .draw_ppu_frame = MmxDrawPpuFrame,
  .hardware_reset = MmxOnHardwareReset,
  /* Savestates carry the title's execution chunk too. */
  .state_save_extra = MmxStateSaveExtra,
  .state_load_extra = MmxStateLoadExtra,
  .on_state_loaded = MmxOnStateLoaded,
};

bool Mmx3dsLoad(const char *path, char *error, size_t cap) {
  if (loaded) { snprintf(error, cap, TR("El juego ya esta cargado.", "The game is already loaded.")); return false; }
  FILE *f = fopen(path, "rb");
  if (!f) { snprintf(error, cap, TR("No se pudo abrir mmx.sfc: %s", "Could not open mmx.sfc: %s"), path); return false; }
  if (fseek(f, 0, SEEK_END)) { fclose(f); return false; }
  long bytes = ftell(f); rewind(f);
  if (bytes != 1572864 && bytes != 1573376) {
    fclose(f); snprintf(error, cap, TR("ROM: se necesita Mega Man X USA Rev 1.", "ROM: Mega Man X USA Rev 1 is required.")); return false;
  }
  size_t size = 1572864;
  uint8_t *rom = malloc(size);
  if (!rom) { fclose(f); snprintf(error, cap, TR("Sin memoria para la ROM.", "Not enough memory for the ROM.")); return false; }
  if (bytes == 1573376) fseek(f, 512, SEEK_SET);
  bool read_ok = fread(rom, 1, size, f) == size; fclose(f);
  if (!read_ok) { free(rom); snprintf(error, cap, TR("Error al leer la ROM.", "Could not read the ROM.")); return false; }
  uint8_t sha[32]; char hex[65]; sha256_compute(rom, size, sha);
  for (unsigned i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", sha[i]);
  if (strcmp(hex, "b8f70a6e7fb93819f79693578887e2c11e196bdf1ac6ddc7cb924b1ad0be2d32")) {
    free(rom); snprintf(error, cap, TR("ROM diferente: usa USA Rev 1 sin modificaciones.", "Wrong ROM: use an unmodified USA Rev 1.")); return false;
  }
  RtlRegisterGame(&info);
  fprintf(stderr, "MMX3DS: ROM verified, SnesInit\n");
  Snes *snes = SnesInit(rom, (int)size);
  fprintf(stderr, "MMX3DS: SnesInit=%p\n", (void *)snes);
  free(rom);
  if (!snes) { snprintf(error, cap, TR("No se pudo iniciar el runtime.", "Could not start the runtime.")); return false; }
  native_audio=true;
#ifndef __3DS__
  native_audio=!(getenv("MMX_AUDIO_LEGACY")&&atoi(getenv("MMX_AUDIO_LEGACY")));
#endif
  dsp_setOutputSpill(native_audio?spill_audio:NULL);
  MmxOnHardwareReset();
  fprintf(stderr, "MMX3DS: reset ready\n");
  MmxRendererReset();
  MmxRendererSetVramSnapshotReuse(true);
  MmxRendererSetStockLineSkipping(true);
  /* Boss Rush is a desktop mod (off unless its plugin is activated); this
   * build has no mod loader, so its default would add a fourth title row
   * drawn by the slow reference compositor. Keep the original title. */
  MmxBossRushSetEnabled(false);
  MmxRendererSetRom(snes->cart->rom, (size_t)snes->cart->romSize);
  MmxZeroSetCollisionRom(snes->cart->rom, (size_t)snes->cart->romSize);
  MmxWeaponsCollisionRom(snes->cart->rom, (size_t)snes->cart->romSize);
  MmxCoopViewsSetOnline(false); MmxRendererSetPeerView(-1);
  g_mmx_custom_renderer = true;
  g_mmx_custom_view = view;
  g_mmx_custom_hud = true;
  g_mmx_expanded_sprites = false;
  g_mmx_render_asset_repairs = false;
  snes_set_hdma_beam_enabled(snes, false);
  RtlSetAudioOutputRate(MMX3DS_RATE);
  RtlEnableExtendedFrameTiming();
  fprintf(stderr, "MMX3DS: core ready\n");
  MmxProfileEnable(getenv("MMX_PROFILE") && atoi(getenv("MMX_PROFILE")));
  MmxAudioRawReset();
  { /* Revision 18: MSU-1 pack beside the ROM (MMX_MSU_DIR overrides). */
    char dir[256]; snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/'); if (slash) *slash = 0; else snprintf(dir, sizeof dir, ".");
#ifndef __3DS__
    if (getenv("MMX_MSU_DIR")) snprintf(dir, sizeof dir, "%s", getenv("MMX_MSU_DIR"));
#endif
    unsigned tracks = Mmx3dsMsuInit(dir);
    if (tracks) { Mmx3dsMsuInstallHooks(); fprintf(stderr, "MMX3DS: MSU-1 pack '%s' tracks=%u\n", Mmx3dsMsuPackName(), tracks); }
  }
  loaded = true;
  if (cap) error[0] = 0;
  return true;
}

bool Mmx3dsRunLogic(uint16_t input) {
  if (!loaded) return false;
  MmxRendererHoldFrame(false);
  MmxRendererLatchSprites();
  ppudma_set_frame_phase(0);
  double start = now_ms();
  MmxProfileFrameBegin();
  { MMX_PROFILE_SCOPE(MMXP_GAME); RtlRunFrame(input); }
  Mmx3dsMsuFrame();
  timing.logic_ms = now_ms() - start;
  return !g_watchdog_tripped;
}

/* The PPU frame always runs: its HDMA and IRQ handlers are game code. Only
 * pixel output and the renderer's snapshot depend on presentation. With
 * present == false the previous capture stays intact, so another core may
 * still be drawing it. With present == true the caller must first make sure
 * nothing is reading the previous capture. */
bool Mmx3dsRunRaster(bool present) {
  if (!loaded) return false;
  MMX_PROFILE_SCOPE(MMXP_PPU);
  double start = now_ms();
  PpuBeginDrawing(g_ppu, (uint8_t *)stock, 256 * sizeof(uint32_t), kPpuRenderFlags_NewRenderer);
  PpuSetExtraSpace(g_ppu, 0);
  if (present) MmxRendererBeginFrame(g_ram);
  MmxRendererSetCaptureEnabled(present);
  ppudma_set_frame_phase(1);
  MmxDrawPpuFrame();
  ppudma_set_frame_phase(0);
  MmxRendererSetCaptureEnabled(true);
  if (present) captured = MmxRendererEndFrame(stock);
  else MmxRendererNoteHiddenFrame(g_ppu);
  timing.raster_ms = now_ms() - start;
  return present ? captured : true;
}

bool Mmx3dsStep(uint16_t input) {
  return Mmx3dsRunLogic(input) && Mmx3dsRunRaster(true);
}

bool Mmx3dsPairIsNative(void) {
  return captured && MmxRendererStereoPairIsNative(view);
}

bool Mmx3dsDraw(uint32_t *out, MmxStereoEye eye, unsigned amount) {
  return captured && MmxRendererDrawEye(out, view, true, eye, amount);
}

bool Mmx3dsDrawPair(uint32_t *left, uint32_t *right, unsigned amount) {
  double start = now_ms();
  bool ok = captured && MmxRendererDrawStereoPair(left, right, view, true, amount);
  timing.stereo_ms = now_ms() - start;
  return ok;
}

void Mmx3dsAudio(int16_t *pcm, unsigned samples) {
  { MMX_PROFILE_SCOPE(MMXP_AUDIO); RtlRenderAudio(pcm, (int)samples, 2); }
}
unsigned Mmx3dsFrame(void) { return (unsigned)snes_frame_counter; }
double Mmx3dsFramePeriods(void) { return RtlLastFramePeriods(); }
void Mmx3dsShutdown(void) {
  Mmx3dsMsuShutdown();
  dsp_setOutputSpill(NULL);MmxNativeFree(&native_backlog);native_audio=false;
  if (g_snes) snes_free(g_snes);
  g_snes = NULL; g_ppu = NULL; g_dma = NULL; g_rom = NULL;
  loaded = captured = false;
}

/* 1.0.2: inventory for the bottom-screen weapons page (RAM map of the USA
 * Rev 1 game, checked against its own code: sub-tanks hold 14 units). */
bool Mmx3dsReadStatus(Mmx3dsStatus *s) {
  memset(s, 0, sizeof *s);
  if (!loaded || !g_ram) return false;
  /* $D1/$D2 = 2/4: inside a stage, including the pause menu, death and
   * stage clear (the renderer's stage-scene test excludes the menu). */
  s->valid = g_ram[0xd1] == 2 && g_ram[0xd2] == 4;
  s->hp = g_ram[0xbcf]; s->hp_max = g_ram[0x1f9a]; s->lives = g_ram[0x1f80];
  s->weapon = g_ram[0xbdb] >> 1;
  for (int i = 0; i < 8; ++i) {
    uint8_t v = g_ram[0x1f88 + 2 * i];
    s->owned[i] = (v & 0x40) != 0; s->ammo[i] = v & 0x3f;
  }
  for (int i = 0; i < 4; ++i) {
    uint8_t v = g_ram[0x1f83 + i];
    s->tank_owned[i] = (v & 0x80) != 0; s->tank[i] = v & 0x0f;
  }
  for (unsigned b = g_ram[0x1f9c]; b; b &= b - 1) ++s->hearts;
  s->upgrades = g_ram[0x1f99] & 0x0f;
  return true;
}

void Mmx3dsLogState(FILE *f) {
  extern unsigned long long g_interp_bridge_bounces, g_interp_bridge_steps;
  if (!f) return;
  fprintf(f, "STATE frame=%u mode=%02x/%02x/%02x stage=%02x xy=%u,%u vram_copies=%u aot_calls=%llu lle_ops=%llu\n",
      Mmx3dsFrame(), g_ram[0xd1], g_ram[0xd2], g_ram[0xd3], g_ram[0x1f7a],
      g_ram[0xbad] | g_ram[0xbae] << 8, g_ram[0xbb0] | g_ram[0xbb1] << 8,
      MmxRendererVramCopies(), g_interp_bridge_bounces, g_interp_bridge_steps);
}

/* ---- Savestates (revision 13) ------------------------------------------- */
#include "crc32.h"
#include <time.h>
#define STATE_MAGIC 0x5333584du /* "MX3S" */
enum { STATE_VERSION = 1 };
typedef struct StateHeader {
  uint32_t magic, version, build, size, crc;
  uint8_t stage, pad[3];
  int64_t time;
  uint16_t thumb[MMX3DS_THUMB_W * MMX3DS_THUMB_H];
} StateHeader;
/* States only load into a build with the same snapshot layout (it follows
 * the runtime and the generated code). v1.0.1 changed presentation only, so
 * it also accepts the states of the released v1.0.0 build. */
static uint32_t state_id(const char *id) {
  return crc32_compute((const uint8_t *)id, strlen(id)) ^ (uint32_t)sizeof(StateHeader);
}
static uint32_t build_id(void) { return state_id("mmx3ds-v1.0.1"); }
static bool build_compatible(uint32_t build) {
  return build == build_id() || build == state_id("mmx3ds-v1.0.0 Nov  9 2026 16:15:10");
}
uint8_t Mmx3dsStage(void) { return loaded ? g_ram[0x1f7a] : 0; }

void Mmx3dsMakeThumb(const uint32_t *image, uint16_t *thumb) {
  /* 256 -> 88 columns (x2.91); the same scale picks a 64-row centre band. */
  for (unsigned y = 0; y < MMX3DS_THUMB_H; ++y) {
    const uint32_t *row = image + (80 + y * 64 / MMX3DS_THUMB_H) * 256;
    for (unsigned x = 0; x < MMX3DS_THUMB_W; ++x) {
      uint32_t c = row[x * 256 / MMX3DS_THUMB_W];
      thumb[y * MMX3DS_THUMB_W + x] = (uint16_t)(((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x1f));
    }
  }
}

bool Mmx3dsSaveState(const char *path, const uint16_t *thumb, char *error, size_t cap) {
  if (!loaded) { snprintf(error, cap, TR("Juego no cargado", "Game not loaded")); return false; }
  size_t capacity = RtlSaveSnapshotToMemory(NULL, 0);
  void *blob = capacity ? malloc(capacity) : NULL;
  size_t size = blob ? RtlSaveSnapshotToMemory(blob, capacity) : 0;
  if (!size) { free(blob); snprintf(error, cap, TR("No se pudo capturar el estado", "Could not capture the state")); return false; }
  StateHeader *h = calloc(1, sizeof *h);
  if (!h) { free(blob); snprintf(error, cap, TR("Sin memoria", "Out of memory")); return false; }
  h->magic = STATE_MAGIC; h->version = STATE_VERSION; h->build = build_id();
  h->size = (uint32_t)size; h->crc = crc32_compute(blob, size);
  h->stage = g_ram[0x1f7a]; h->time = (int64_t)time(NULL);
  if (thumb) memcpy(h->thumb, thumb, sizeof h->thumb);
  char temp[256]; snprintf(temp, sizeof temp, "%s.tmp", path);
  FILE *f = fopen(temp, "wb");
  uint8_t msu[MMX3DS_MSU_STATE_SIZE]; Mmx3dsMsuSaveState(msu);
  bool ok = f && fwrite(h, sizeof *h, 1, f) == 1 && fwrite(blob, 1, size, f) == size &&
      fwrite(msu, 1, sizeof msu, f) == sizeof msu;
  if (f && fclose(f)) ok = false;
  free(blob); free(h);
  if (ok) { remove(path); ok = rename(temp, path) == 0; }
  if (!ok) { remove(temp); snprintf(error, cap, TR("No se pudo escribir en la SD", "Could not write to the SD card")); return false; }
  fprintf(stderr, "STATE saved %s bytes=%zu frame=%u stage=%02x\n", path, size, Mmx3dsFrame(), g_ram[0x1f7a]);
  return true;
}

static bool read_header(FILE *f, StateHeader *h) {
  return fread(h, sizeof *h, 1, f) == 1 && h->magic == STATE_MAGIC &&
         h->version == STATE_VERSION;
}

bool Mmx3dsReadStateInfo(const char *path, Mmx3dsStateInfo *info) {
  memset(info, 0, sizeof *info);
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  StateHeader *h = malloc(sizeof *h);
  bool ok = h && read_header(f, h) && build_compatible(h->build);
  if (ok) {
    info->used = true; info->stage = h->stage; info->time = h->time;
    memcpy(info->thumb, h->thumb, sizeof info->thumb);
  }
  free(h); fclose(f);
  return ok;
}

bool Mmx3dsLoadState(const char *path, char *error, size_t cap) {
  if (!loaded) { snprintf(error, cap, TR("Juego no cargado", "Game not loaded")); return false; }
  FILE *f = fopen(path, "rb");
  if (!f) { snprintf(error, cap, TR("Ranura vacia", "Empty slot")); return false; }
  StateHeader *h = malloc(sizeof *h);
  void *blob = NULL;
  bool ok = h && read_header(f, h);
  if (ok && !build_compatible(h->build)) { ok = false; snprintf(error, cap, TR("Estado de otra version", "State from another version")); }
  else if (!ok) snprintf(error, cap, TR("Archivo de estado invalido", "Invalid state file"));
  if (ok) {
    blob = malloc(h->size);
    ok = blob && fread(blob, 1, h->size, f) == h->size && crc32_compute(blob, h->size) == h->crc;
    if (!ok) snprintf(error, cap, TR("Estado danado", "Damaged state"));
  }
  uint8_t msu[MMX3DS_MSU_STATE_SIZE];
  bool have_msu = ok && fread(msu, 1, sizeof msu, f) == sizeof msu;
  fclose(f);
  if (ok) {
    ok = RtlLoadSnapshotFromMemory(blob, h->size);
    if (!ok) snprintf(error, cap, TR("El runtime rechazo el estado", "The runtime rejected the state"));
  }
  if (ok) {
    /* The old timeline's PCM must not be heard after the jump. */
    Dsp *dsp = g_snes->apu->dsp;
    native_backlog.head = native_backlog.count = 0;
    dsp_advance(dsp, dsp_available(dsp));
    captured = false;
    Mmx3dsMsuLoadState(have_msu ? msu : NULL);
    MmxRendererSetVramSnapshotReuse(true);
    MmxRendererSetStockLineSkipping(true);
    fprintf(stderr, "STATE loaded %s frame=%u stage=%02x\n", path, Mmx3dsFrame(), g_ram[0x1f7a]);
  }
  free(blob); free(h);
  return ok;
}
