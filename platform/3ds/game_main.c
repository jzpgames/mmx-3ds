#include "mmx3ds_audio_metrics.h"
#include "audio_device.h"
#include "mmx3ds_profile.h"
#include <3ds.h>
#include "game_core.h"
#include "framebuffer.h"
#include "mmx_renderer.h"
#include "ui.h"
#include "msu3ds.h"
#include <sys/stat.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

u32 __stacksize__ = 1024 * 1024;
/* Reserve enough linear memory for stereo LCD buffers and NDSP, leaving the
 * remaining process budget to the native game and its ordinary heap. */
u32 __ctru_linear_heap_size = 4 * 1024 * 1024;

static bool sound;

static uint16_t controller(u32 keys) {
  const u32 ctr[] = {KEY_B, KEY_Y, KEY_SELECT, KEY_START,
      KEY_DUP | KEY_CPAD_UP, KEY_DDOWN | KEY_CPAD_DOWN,
      KEY_DLEFT | KEY_CPAD_LEFT, KEY_DRIGHT | KEY_CPAD_RIGHT,
      KEY_A, KEY_X, KEY_L, KEY_R};
  uint16_t snes = 0;
  for (unsigned i = 0; i < 12; ++i) if (keys & ctr[i]) snes |= 1u << i;
  return snes;
}

/* ---- Presentation pipeline (revision 7) -----------------------------------
 * Main thread (core 0): game logic, PPU frame, audio.
 * Composer (core 2):    both eyes of a captured frame into a free slot.
 * Presenter (core 2, higher priority, woken by VBlank): copies the oldest
 *   ready slot to the LCD and swaps, holding each image for the VBlanks its
 *   guest frames cover (2 normally, 3 for a lag frame), so presentation runs
 *   at a steady 30 Hz instead of whenever an image happens to finish.
 * With two slots the game runs one image ahead of the screen: a spike in
 * game logic of up to one image (33 ms) drains that buffer instead of
 * delaying the screen. Cost: about one image of extra latency.
 * The game is paced by free slots, i.e. by the display: 59.83 Hz instead of
 * 60.10 Hz (-0.45 %); the audio rate is scaled by the same factor.
 * The main thread never begins a new capture while the composer still
 * reads the previous one. Reference-path frames are composed in step. */
#ifndef MMX3DS_PRESENTATION_SLOTS
#define MMX3DS_PRESENTATION_SLOTS 2
#endif
enum { SLOTS = MMX3DS_PRESENTATION_SLOTS };
_Static_assert(SLOTS==2||SLOTS==3,"presentation slots must be 2 or 3");
#define LCD_HZ 59.8261
typedef struct RenderJob { int slot; unsigned amount; Mmx3dsDisplay display; float periods; } RenderJob;
static RenderJob job;
static uint32_t *slot_image[SLOTS];
static Mmx3dsDisplay slot_display[SLOTS];
static bool slot_free[SLOTS];
static int last_image = -1; /* slot of the newest composed image */
static float slot_periods[SLOTS];
static int ready_fifo[SLOTS];
static unsigned ready_head, ready_count;
static LightLock slot_lock, gfx_lock;
static LightSemaphore free_slots;
static LightEvent job_ready, job_done;
static Thread worker, presenter;
static volatile bool worker_quit, worker_busy, worker_ok = true;
static volatile bool presenter_pause, presenter_blitting;
static volatile u32 vblanks, swap_vblank;
static volatile u32 worker_images, worker_stereo_us, worker_lcd_us;
static volatile unsigned worker_depth_pixels;
static volatile unsigned worker_depth_layer;
static volatile u32 stereo_peak_us, held_vblanks;
static const char *render_mode = "1 nucleo";

static u32 ticks_us(u64 ticks) { return (u32)(ticks * 1000000ull / SYSCLOCK_ARM11); }
static void count_vblank(void *unused) { (void)unused; ++vblanks; }

static void blit_and_swap(const uint32_t *image, Mmx3dsDisplay display) {
  /* The back buffer is the one shown before the last swap; it is only
   * free once the GSP has latched that swap at a VBlank. */
  while (vblanks == swap_vblank && !worker_quit) gspWaitForVBlank();
  u64 t = svcGetSystemTick();
  LightLock_Lock(&gfx_lock);
  u8 *left = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
  u8 *right = gfxGetFramebuffer(GFX_TOP, GFX_RIGHT, NULL, NULL);
  LightLock_Unlock(&gfx_lock);
  /* Revision 15: the sharp scaler costs more than the old nearest blit, so
   * skip the right eye when it cannot differ: slider at zero (left shown on
   * both) or a pair the composer drew flat (copy the finished panel). */
  const bool stereo = osGet3DSliderState() > 0.0f;
  Mmx3dsBlitTopBgr8(image, left, display);
  if (stereo) {
    if (!memcmp(image, image + 256 * 224, 256 * 224 * sizeof(*image)))
      memcpy(right, left, 400 * 240 * 3);
    else Mmx3dsBlitTopBgr8(image + 256 * 224, right, display);
  }
  GSPGPU_FlushDataCache(left, 400 * 240 * 3);
  if (stereo) GSPGPU_FlushDataCache(right, 400 * 240 * 3);
  LightLock_Lock(&gfx_lock);
  gfxScreenSwapBuffers(GFX_TOP, stereo);
  swap_vblank = vblanks;
  LightLock_Unlock(&gfx_lock);
  worker_lcd_us += ticks_us(svcGetSystemTick() - t);
  ++worker_images;
}

static void release_slot(int s) {
  LightLock_Lock(&slot_lock); slot_free[s] = true; LightLock_Unlock(&slot_lock);
  LightSemaphore_Release(&free_slots, 1);
}

static int acquire_slot(void) {
  LightSemaphore_Acquire(&free_slots, 1);
  LightLock_Lock(&slot_lock);
  int s = 0;
  while (!slot_free[s]) ++s;
  slot_free[s] = false;
  LightLock_Unlock(&slot_lock);
  return s;
}

static void compose_job(void) {
  uint32_t *image = slot_image[job.slot];
  u64 t0 = svcGetSystemTick();
  bool ok = Mmx3dsDrawPair(image, image + 256 * 224, job.amount);
  worker_depth_pixels = MmxRendererStereoDepthPixels(job.amount);
  worker_depth_layer = MmxRendererStereoLayer() + 1;
  u32 us = ticks_us(svcGetSystemTick() - t0);
  worker_stereo_us += us;
  u32 observed=__atomic_load_n(&stereo_peak_us,__ATOMIC_RELAXED);
  while(us>observed&&!__atomic_compare_exchange_n(&stereo_peak_us,&observed,us,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)){}
  worker_ok = ok;
  if (!ok) { release_slot(job.slot); return; }
  slot_display[job.slot] = job.display;
  last_image = job.slot;
  slot_periods[job.slot] = job.periods;
  if (!presenter) {                 /* Single-core fallback: show it now. */
    blit_and_swap(image, job.display);
    release_slot(job.slot);
    return;
  }
  LightLock_Lock(&slot_lock);
  ready_fifo[(ready_head + ready_count) % SLOTS] = job.slot; ++ready_count;
  LightLock_Unlock(&slot_lock);
}

static void worker_main(void *unused) {
  (void)unused;
  for (;;) {
    LightEvent_Wait(&job_ready);
    if (worker_quit) break;
    compose_job();
    LightEvent_Signal(&job_done);
  }
}

static void presenter_main(void *unused) {
  (void)unused;
  u32 next = 0; float owed = 0; bool started = false;
  while (!worker_quit) {
    gspWaitForVBlank();
    if (presenter_pause || worker_quit) continue;
    u32 now = vblanks;
    if (started && (s32)(now - next) < 0) continue;
    LightLock_Lock(&slot_lock);
    int s = -1;
    if (ready_count) { s = ready_fifo[ready_head]; ready_head = (ready_head + 1) % SLOTS; --ready_count; }
    LightLock_Unlock(&slot_lock);
    if (s < 0) { if (started) ++held_vblanks; continue; }
    presenter_blitting = true;
    if (!presenter_pause) blit_and_swap(slot_image[s], slot_display[s]);
    presenter_blitting = false;
    /* Hold this image for the VBlanks its guest frames cover. */
    owed += slot_periods[s];
    unsigned hold = (unsigned)(owed + 0.5f); if (hold < 1) hold = 1;
    owed -= hold;
    next = now + hold; started = true;
    release_slot(s);
  }
}

static void start_worker(void) {
  LightEvent_Init(&job_ready, RESET_ONESHOT);
  LightEvent_Init(&job_done, RESET_ONESHOT);
  LightLock_Init(&gfx_lock); LightLock_Init(&slot_lock);
  LightSemaphore_Init(&free_slots, SLOTS, SLOTS);
  for (unsigned i = 0; i < SLOTS; ++i) {
    slot_image[i] = malloc(2 * 256 * 224 * sizeof(uint32_t));
    slot_free[i] = slot_image[i] != NULL;
  }
  gspSetEventCallback(GSPGPU_EVENT_VBlank0, count_vblank, NULL, false);
  s32 priority = 0x30;
  svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
  /* draw_frame keeps ~280 KB on its stack; leave generous headroom. */
  const size_t stack = 1024 * 1024;
  worker = threadCreate(worker_main, NULL, stack, priority, 2, false);
  if (worker) {
    render_mode = "nucleo 2";
    presenter = threadCreate(presenter_main, NULL, 64 * 1024,
        priority > 0x19 ? priority - 1 : priority, 2, false);
    if (presenter) render_mode = "nucleo 2 + cola";
    return;
  }
  /* Old 3DS / no third core: the system core can lend up to 80 %. */
  if (R_SUCCEEDED(APT_SetAppCpuTimeLimit(80)))
    worker = threadCreate(worker_main, NULL, stack, priority, 1, false);
  if (worker) render_mode = "nucleo 1 (80%)";
}

static void wait_worker(void) {
  if (!worker_busy) return;
  LightEvent_Wait(&job_done);
  worker_busy = false;
}

/* Blocks until a slot is free: this is what paces the game to the display
 * when the presenter runs. */
static void submit_render(unsigned amount, Mmx3dsDisplay display, float periods, bool in_step) {
  job = (RenderJob){acquire_slot(), amount, display, periods};
  if (!worker) { compose_job(); return; }
  worker_busy = true;
  LightEvent_Signal(&job_ready);
  if (in_step) wait_worker();
}

/* HOME menu, sleep and exit release the GPU right on the main thread;
 * nothing may be presenting at that moment. */
static aptHookCookie apt_cookie;
static void on_apt(APT_HookType hook, void *unused) {
  (void)unused;
  if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP || hook == APTHOOK_ONEXIT) {
    if(sound)MmxAudioPause(true);
    wait_worker();
    presenter_pause = true;
    while (presenter_blitting) svcSleepThread(1000000);
  } else if (hook == APTHOOK_ONRESTORE || hook == APTHOOK_ONWAKEUP) {
    presenter_pause = false;
    if(sound)MmxAudioPause(false);
  }
}

static void stop_worker(void) {
  if (!worker) return;
  wait_worker();
  worker_quit = true;
  LightEvent_Signal(&job_ready);
  threadJoin(worker, U64_MAX); threadFree(worker); worker = NULL;
  if (presenter) { threadJoin(presenter, U64_MAX); threadFree(presenter); presenter = NULL; }
  for (unsigned i = 0; i < SLOTS; ++i) free(slot_image[i]);
}

static void wait_exit(void) {
  printf("\nPulsa START para salir.\n");
  while (aptMainLoop()) {
    hidScanInput(); if (hidKeysDown() & KEY_START) break;
    gfxFlushBuffers();
    gspWaitForVBlank();
  }
}

/* ---- Bottom screen, savestates and settings (revision 13) --------------- */
#define DATA_DIR "sdmc:/3ds/mmx-3ds"
static double now_ms(void) { return svcGetSystemTick() * (1000.0 / SYSCLOCK_ARM11); }
static void slot_path(int slot, char *out, size_t n) {
  snprintf(out, n, DATA_DIR "/states/slot%d.sav", slot + 1);
}
static Mmx3dsDisplay display = MMX3DS_DISPLAY_WIDE;
/* Revision 18: MSU-1 switch and volumes (percent, steps of 10). */
static bool msu_on = true; static unsigned music_vol = 80, sfx_vol = 100;
static void apply_audio(void) {
  Mmx3dsMsuSetEnabled(msu_on); Mmx3dsMsuSetGains(music_vol, sfx_vol);
  UiSetAudio(Mmx3dsMsuTracks() > 0, msu_on, music_vol, sfx_vol);
}
static void load_settings(void) {
  FILE *f = fopen(DATA_DIR "/ui.cfg", "rb");
  unsigned char cfg[5] = {MMX3DS_DISPLAY_WIDE, 0, 1, 80, 100};
  if (f) { size_t n = fread(cfg, 1, 5, f); if (n < 2) cfg[0] = cfg[1] = 0;
    if (n < 5) { cfg[2] = 1; cfg[3] = 80; cfg[4] = 100; } fclose(f); }
  display = cfg[0] < MMX3DS_DISPLAY_COUNT ? (Mmx3dsDisplay)cfg[0] : MMX3DS_DISPLAY_WIDE;
  UiSetDisplay(display); UiSetSelected(cfg[1] < UI_SLOTS ? cfg[1] : 0);
  msu_on = cfg[2] != 0; music_vol = cfg[3] <= 150 ? cfg[3] : 80; sfx_vol = cfg[4] <= 150 ? cfg[4] : 100;
  apply_audio();
}
static void save_settings(void) {
  FILE *f = fopen(DATA_DIR "/ui.cfg", "wb");
  if (!f) return;
  unsigned char cfg[5] = {(unsigned char)display, (unsigned char)UiSelected(), msu_on, (unsigned char)music_vol, (unsigned char)sfx_vol};
  fwrite(cfg, 1, 5, f); fclose(f);
}
static void refresh_slots(void) {
  for (int i = 0; i < UI_SLOTS; ++i) {
    char path[96]; slot_path(i, path, sizeof path);
    Mmx3dsStateInfo info;
    UiSetSlot(i, Mmx3dsReadStateInfo(path, &info) ? &info : NULL);
  }
}
static void present_ui(void) {
  if (!UiRender(now_ms())) return;
  LightLock_Lock(&gfx_lock);
  u16 *fb = (u16 *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
  LightLock_Unlock(&gfx_lock);
  UiPresent(fb);
  GSPGPU_FlushDataCache(fb, 320 * 240 * 2);
}
/* Runs between stereo pairs: no guest frame is in flight and the composer
 * has finished, so the snapshot sees a consistent machine. */
static void state_action(bool load, int slot) {
  static uint16_t thumb[MMX3DS_THUMB_W * MMX3DS_THUMB_H];
  char path[96], error[96], message[64];
  slot_path(slot, path, sizeof path);
  wait_worker();
  if (sound) MmxAudioPause(true);
  u64 t = svcGetSystemTick();
  bool ok;
  if (load) {
    ok = Mmx3dsLoadState(path, error, sizeof error);
    if (ok && sound) MmxAudioFlush();
  } else {
    if (last_image >= 0) Mmx3dsMakeThumb(slot_image[last_image], thumb);
    else memset(thumb, 0, sizeof thumb);
    ok = Mmx3dsSaveState(path, thumb, error, sizeof error);
    if (ok) { Mmx3dsStateInfo info; if (Mmx3dsReadStateInfo(path, &info)) UiSetSlot(slot, &info); }
  }
  if (sound) MmxAudioPause(false);
  fprintf(stderr, "STATE %s slot=%d ok=%d ms=%.1f %s\n", load ? "load" : "save", slot + 1, ok,
          ticks_us(svcGetSystemTick() - t) / 1000.0, ok ? "" : error);
  if (ok) snprintf(message, sizeof message, "✓ %s ranura %d", load ? "Cargada" : "Guardado en", slot + 1);
  else snprintf(message, sizeof message, "✗ %s", error);
  UiSetSelected(slot); save_settings();
  UiToast(message, ok, now_ms());
}

int main(int argc, char **argv) {
  gfxInitDefault(); gfxSet3D(true); consoleInit(GFX_BOTTOM, NULL);
  FILE *log = freopen("sdmc:/3ds/mmx-3ds/mmx-log.txt", "w", stderr);
  if (log) setvbuf(log, NULL, _IOLBF, 4096);
  fprintf(stderr, "MMX3DS: main, stereo LCD ready\n");
  osSetSpeedupEnable(true);
  /* osSetSpeedupEnable has no return value. Record an explicit request so
   * a physical-device report can distinguish a failed clock/cache request. */
  bool new_model = false; Result model_rc = APT_CheckNew3DS(&new_model);
  Result boost_rc = ptmSysmInit();
  if (R_SUCCEEDED(boost_rc)) {
    boost_rc = PTMSYSM_ConfigureNew3DSCPU(3); ptmSysmExit();
  }
  fprintf(stderr, "BOOST new3ds=%d model_rc=%08lx request_rc=%08lx\n",
      new_model, (unsigned long)model_rc, (unsigned long)boost_rc);
  printf("Mega Man X 3DS\nVersion experimental\n\nCargando ROM USA Rev 1...\n");
  char rom[1024] = "sdmc:/3ds/mmx-3ds/mmx.sfc";
  if (argc > 0 && argv && argv[0]) {
    char *slash = strrchr(argv[0], '/');
    if (slash && (size_t)(slash - argv[0]) + sizeof("/mmx.sfc") < sizeof(rom))
      snprintf(rom, sizeof(rom), "%.*s/mmx.sfc", (int)(slash - argv[0]), argv[0]);
  }
  char error[1024] = "Error al cargar el juego.";
  fprintf(stderr, "MMX3DS: loading %s\n", rom);
  if (!Mmx3dsLoad(rom, error, sizeof(error))) {
    printf("\n%s\n\nColoca tu ROM USA Rev 1 como\nmmx.sfc junto al archivo 3dsx.\n", error);
    wait_exit(); gfxExit(); return 1;
  }
  sound = MmxAudioStart();
  if(!sound){printf("Audio NDSP no disponible. Revisa /3ds/dspfirm.cdc.\n");wait_exit();Mmx3dsShutdown();gfxExit();return 1;}
  MmxProfileEnable(false);
  fprintf(stderr, "MMX3DS: loaded, audio=%d\n", sound);
  consoleClear();
  mkdir(DATA_DIR, 0777); mkdir(DATA_DIR "/states", 0777);
  UiInit(); UiSetNow((int64_t)time(NULL)); load_settings(); refresh_slots();
  { UiStats initial = {0, 0, 0, 0, 0, true, false}; UiSetStats(&initial); }
  present_ui();
  start_worker();
  if(presenter)MmxAudioSetBaseRate(MMX3DS_NATIVE_RATE*(LCD_HZ/60.0988));
  aptHook(&apt_cookie,on_apt,NULL);
  fprintf(stderr,"RENDER mode=%s slots=%d depth=revision10 profile_default=off\n",render_mode,SLOTS);
  bool ok = true;
  int pending = -1; bool pending_load = false;
  u32 audio_issue_base = 0;
  const double frame_ticks = SYSCLOCK_ARM11 / 60.0988;
  u64 fps_start = svcGetSystemTick();
  unsigned fps_guest_start = Mmx3dsFrame();
  u32 images_start = worker_images, stereo_start = worker_stereo_us, lcd_start = worker_lcd_us;
  double cpu_ms = 0, ppu_ms = 0, wait_ms = 0, main_peak_ms = 0;
  unsigned measured_guest = 0, pairs = 0;
  u32 held_start = held_vblanks;
  double lifetime_main_peak=0;u32 lifetime_stereo_peak=0;
  u32 held_base=held_vblanks,ndsp_drop_base=ndspGetDroppedFrames();u64 window_start=fps_start;
  double deadline = (double)fps_start;
  unsigned amount = 0;
  while (aptMainLoop()) {
    hidScanInput(); u32 keys = hidKeysHeld();
    const u32 exit_keys = KEY_L | KEY_R | KEY_START | KEY_SELECT;
    if ((keys & exit_keys) == exit_keys) break;
    u32 down = hidKeysDown();
    /* SELECT + L saves, SELECT + R loads the selected slot. L and R do not
     * reach the game while SELECT is held. */
    if ((keys & KEY_SELECT) && !(keys & KEY_START)) {
      if ((down & KEY_L) && !(keys & KEY_R)) { pending = UiSelected(); pending_load = false; }
      if ((down & KEY_R) && !(keys & KEY_L)) {
        pending = UiSelected(); pending_load = true;
        if (!UiSlotUsed(pending)) {
          char m[48]; snprintf(m, sizeof m, "✗ Ranura %d vacía", pending + 1);
          UiToast(m, false, now_ms()); pending = -1;
        }
      }
    }
    if (down & KEY_TOUCH) {
      touchPosition touch;hidTouchRead(&touch);
      UiAction action = UiTouch(touch.px, touch.py, now_ms());
      if (action.kind == UI_SAVE || action.kind == UI_LOAD) { pending = action.arg; pending_load = action.kind == UI_LOAD; }
      else if (action.kind == UI_DISPLAY) { display = (Mmx3dsDisplay)action.arg; save_settings(); }
      else if (action.kind == UI_FILTER) MmxAudioToggleInterp();
      else if (action.kind == UI_MUSIC_SOURCE) {
        if (msu_on != (action.arg != 0)) {
          msu_on = action.arg != 0; apply_audio(); save_settings();
          UiToast(msu_on ? "MSU-1: próxima canción" : "SNES: próxima canción", true, now_ms());
        }
      }
      else if (action.kind == UI_MUSIC_VOL || action.kind == UI_SFX_VOL) {
        unsigned *v = action.kind == UI_MUSIC_VOL ? &music_vol : &sfx_vol;
        int next = (int)*v + action.arg * 10; if (next < 0) next = 0; if (next > 150) next = 150;
        *v = (unsigned)next; apply_audio(); save_settings();
        char m[48]; snprintf(m, sizeof m, "%s %u%%", action.kind == UI_MUSIC_VOL ? "Música" : "Efectos", *v);
        UiToast(m, true, now_ms());
      }
      else if (action.kind == UI_PROFILE) { MmxProfileEnable(!g_mmx_profile_enabled); fprintf(stderr,"PROFILE enabled=%d\n",g_mmx_profile_enabled); }
      else if (action.kind == UI_RESET) {window_start=svcGetSystemTick();held_base=held_vblanks;held_start=held_base;
        lifetime_main_peak=main_peak_ms=0;lifetime_stereo_peak=0;
        __atomic_exchange_n(&stereo_peak_us,0,__ATOMIC_RELAXED);
        ndsp_drop_base=ndspGetDroppedFrames();
        MmxAudioResetMetrics();memset(&g_mmx_audio_core_metrics,0,sizeof g_mmx_audio_core_metrics);
        (void)MmxProfileTake();fprintf(stderr,"MEASUREMENT_RESET frame=%u profile=%d\n",Mmx3dsFrame(),g_mmx_profile_enabled);
        audio_issue_base=0;UiToast("✓ Mediciones reiniciadas", true, now_ms());}
    }
    if (pending >= 0) { state_action(pending_load, pending); pending = -1; }
    present_ui();
    uint16_t pad = controller((keys & KEY_SELECT) ? keys & ~(KEY_L | KEY_R) : keys);
    u64 pair_start = svcGetSystemTick(); double pair_wait = 0, pair_periods = 0;
    /* Two guest frames per stereo pair (about 30 Hz), at the original
     * cadence: every frame runs its logic, HDMA, IRQ and audio. Only the
     * second is captured and shown; the first skips all pixel work. */
    for (unsigned step = 0; step < 2 && ok; ++step) {
      bool present = step == 1;
      ok = Mmx3dsRunLogic(pad);
      if (!ok) break;
      if (present) {
        u64 w = svcGetSystemTick();
        wait_worker();
        pair_wait = (svcGetSystemTick() - w) * (1000.0 / SYSCLOCK_ARM11);
        wait_ms += pair_wait;
        if (!worker_ok) { ok = false; break; }
      }
      ok = Mmx3dsRunRaster(present);
      if (Mmx3dsFrame() <= 3) fprintf(stderr, "MMX3DS: frame %u step=%d\n", Mmx3dsFrame(), ok);
      if (!ok) break;
      Mmx3dsFrameTiming sample = Mmx3dsTiming();
      cpu_ms += sample.logic_ms; ppu_ms += sample.raster_ms; ++measured_guest;
      double periods = Mmx3dsFramePeriods();
      deadline += frame_ticks * periods; pair_periods += periods;
      unsigned samples=Mmx3dsAudioAvailable();
      if(!MmxAudioSubmit(samples)){ok=false;break;}
    }
    if (!ok) break;
    float slider = osGet3DSliderState();
    if (!(slider > 0)) slider = 0;
    if (slider > 1) slider = 1;
    amount = (unsigned)(slider * MMX_STEREO_FULL + 0.5f);
    /* Native-path frames overlap the next guest frames; others are drawn
     * before continuing, since the reference path reads more live state. */
    double pair_ms = (svcGetSystemTick() - pair_start) * (1000.0 / SYSCLOCK_ARM11) - pair_wait;
    if (pair_ms > main_peak_ms) main_peak_ms = pair_ms;
    submit_render(amount, display, (float)pair_periods, !Mmx3dsPairIsNative());
    if (!worker && !worker_ok) { ok = false; break; }
    ++pairs;
    if (!presenter) {
      /* No presenter: pace to the guest's own clock, sleeping only when
       * ahead, and drop accumulated lag rather than spiral. */
      double now = (double)svcGetSystemTick();
      if (now < deadline) svcSleepThread((s64)((deadline - now) * (1e9 / SYSCLOCK_ARM11)));
      else if (now > deadline + frame_ticks * 3) deadline = now;
    }
    u64 tick = svcGetSystemTick();
    if (tick - fps_start >= SYSCLOCK_ARM11) {
      double seconds = (tick - fps_start) / (double)SYSCLOCK_ARM11;
      /* Revision 14: snapshot the presenter counters once. Re-reading them
       * after the slow logging below lost every image presented meanwhile
       * (about one per window: rev 12 reported 28.9 FPS for a steady 29.9). */
      const u32 images_now = worker_images, stereo_now = worker_stereo_us;
      const u32 lcd_now = worker_lcd_us, held_now = held_vblanks;
      u32 images = images_now - images_start;
      double stereo = images ? (stereo_now - stereo_start) / 1000.0 / images : 0;
      double lcd = images ? (lcd_now - lcd_start) / 1000.0 / images : 0;
      double cpu = cpu_ms / measured_guest, ppu = ppu_ms / measured_guest;
      double wait = pairs ? wait_ms / pairs : 0;
      MmxProfileStats subs=MmxProfileTake();
      MmxAudioStatus audio=MmxAudioGetStatus();
      if(main_peak_ms>lifetime_main_peak)lifetime_main_peak=main_peak_ms;
      u32 peak3d=__atomic_exchange_n(&stereo_peak_us,0,__ATOMIC_RELAXED);
      if(peak3d>lifetime_stereo_peak)lifetime_stereo_peak=peak3d;
      {
        static char lines[UI_DETAIL_LINES][UI_LINE];
        static const char *names[]={"Juego ","Puente","SPC   ","DSP   ","PPU   ","PCM   "};
        memset(lines,0,sizeof lines);
        snprintf(lines[0],UI_LINE,"Juego %.1f  Imagen %.1f FPS  (%s)",(Mmx3dsFrame()-fps_guest_start)/seconds,images/seconds,render_mode);
        snprintf(lines[1],UI_LINE,"3D %u%%  capa BG%u  +/- %u px",amount*100/MMX_STEREO_FULL,worker_depth_layer,worker_depth_pixels);
        snprintf(lines[2],UI_LINE,"CPU %.1f  PPU %.1f  3D %.1f  LCD %.1f  espera %.1f ms",cpu,ppu,stereo,lcd,wait);
        snprintf(lines[3],UI_LINE,"Pico juego %.1f  3D %.1f   Max juego %.1f  3D %.1f",main_peak_ms,peak3d/1000.0,lifetime_main_peak,lifetime_stereo_peak/1000.0);
        snprintf(lines[4],UI_LINE,"VBlank sin imagen %lu",(unsigned long)(held_vblanks-held_base));
        snprintf(lines[5],UI_LINE,"Audio U:%u O:%u perdidas:%u",audio.underruns+(unsigned)g_mmx_audio_core_metrics.render_underruns,audio.overruns,audio.dropped+(unsigned)g_mmx_audio_core_metrics.dsp_dropped);
        snprintf(lines[6],UI_LINE,"Cola %.0f ms (min %.0f)  %s %+.3f%%  %s",audio.queued*1000.0/audio.rate,audio.min_queued*1000.0/audio.rate,audio.polyphase?"Polifase":"Lineal",audio.correction*100,audio.priming?"cargando":"activo");
        snprintf(lines[7],UI_LINE,"Desglose CPU %s (medio / max por paso)",g_mmx_profile_enabled?"SÍ":"NO");
        for(unsigned c=0;c<MMXP_COUNT&&8+c<UI_DETAIL_LINES-1;++c)
          snprintf(lines[8+c],UI_LINE,"  %s %6.2f / %6.2f ms",names[c],subs.frames?subs.ms[c]/subs.frames:0,subs.peak_ms[c]);
        snprintf(lines[UI_DETAIL_LINES-2],UI_LINE,"Reloj %.0f ns  lecturas %llu",subs.clock_ns,(unsigned long long)subs.reads);
        UiSetDetail(lines);
        u32 issues=audio.underruns+audio.dropped+(u32)g_mmx_audio_core_metrics.render_underruns+(u32)g_mmx_audio_core_metrics.dsp_dropped;
        UiStats st={(float)(images/seconds),amount*100/MMX_STEREO_FULL,(float)((cpu+ppu)*2),(float)stereo,
                    issues-audio_issue_base,audio.polyphase,g_mmx_profile_enabled};
        audio_issue_base=issues;
        UiSetStats(&st); UiSetNow((int64_t)time(NULL));
      }
      Mmx3dsLogState(stderr);
      fprintf(stderr,"PERF frame=%u span_s=%.3f window_s=%.3f profile=%d images_s=%.2f cpu_ms=%.3f ppu_ms=%.3f stereo_ms=%.3f lcd_ms=%.3f peak_main_ms=%.3f peak_3d_ms=%.3f max_main_ms=%.3f max_3d_ms=%.3f missed_vblanks=%lu missed_total=%lu depth=%u BG%u amount=%u\n",
        Mmx3dsFrame(),seconds,(tick-window_start)/(double)SYSCLOCK_ARM11,g_mmx_profile_enabled,images/seconds,cpu,ppu,stereo,lcd,main_peak_ms,peak3d/1000.0,lifetime_main_peak,lifetime_stereo_peak/1000.0,(unsigned long)(held_now-held_start),(unsigned long)held_now,worker_depth_pixels,worker_depth_layer,amount);
      fprintf(stderr,"SUBSYSTEM frames=%u clock_ns=%.1f reads=%llu",subs.frames,subs.clock_ns,(unsigned long long)subs.reads);
      for(unsigned c=0;c<MMXP_COUNT;++c)fprintf(stderr," c%u_ms=%.4f c%u_peak=%.4f c%u_calls=%llu",c,subs.frames?subs.ms[c]/subs.frames:0,c,subs.peak_ms[c],c,(unsigned long long)subs.calls[c]);
      fprintf(stderr,"\nAUDIO underruns=%u overruns=%u lost=%u queued=%u min=%u max=%u rate=%.4f adjust=%.6f priming=%d filter=%s generated=%llu submitted=%llu completed=%llu backpressure_ms=%.3f callback_peak_us=%.2f ndsp_dropped=%lu ndsp_dropped_window=%lu\n",audio.underruns,audio.overruns,audio.dropped,audio.queued,audio.min_queued,audio.max_queued,audio.rate,audio.correction,audio.priming,audio.polyphase?"polyphase":"linear",(unsigned long long)audio.generated,(unsigned long long)audio.submitted,(unsigned long long)audio.completed,audio.backpressure_ms,audio.callback_peak_ticks*(1e6/SYSCLOCK_ARM11),(unsigned long)ndspGetDroppedFrames(),(unsigned long)(ndspGetDroppedFrames()-ndsp_drop_base));
      fprintf(stderr,"CORE_AUDIO ring_drops=%llu render_underruns=%llu missing_frames=%llu priming_frames=%llu spill_events=%llu spill_frames=%llu backlog_peak=%llu native_delivered=%llu\n",(unsigned long long)g_mmx_audio_core_metrics.dsp_dropped,(unsigned long long)g_mmx_audio_core_metrics.render_underruns,(unsigned long long)g_mmx_audio_core_metrics.render_missing,(unsigned long long)g_mmx_audio_core_metrics.priming_frames,(unsigned long long)g_mmx_audio_core_metrics.spill_events,(unsigned long long)g_mmx_audio_core_metrics.spill_frames,(unsigned long long)g_mmx_audio_core_metrics.backlog_peak,(unsigned long long)g_mmx_audio_core_metrics.native_delivered);
      fps_start = tick; fps_guest_start = Mmx3dsFrame();
      images_start = images_now; stereo_start = stereo_now; lcd_start = lcd_now;
      cpu_ms = ppu_ms = wait_ms = main_peak_ms = 0; measured_guest = pairs = 0;
      held_start = held_now;
    }
  }
  stop_worker();
  aptUnhook(&apt_cookie);
  if (sound) MmxAudioStop();
  Mmx3dsShutdown();
  if (!ok) { consoleClear(); printf("\nEl runtime se detuvo.\n"); wait_exit(); }
  gfxExit(); return ok ? 0 : 1;
}
