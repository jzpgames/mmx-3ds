/* MSU-1 music for the 3DS port (revision 18). See msu3ds.h. */
#include "msu3ds.h"
#include "game_core.h"
#include "common_rtl.h"
#include "snes/interp_bridge.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifdef __3DS__
#include <3ds.h>
#endif

#define MSU_RATE 44100
#define VOLUME_FULL 0x50   /* DarkShock's emulator levels, relative */
#define VOLUME_DUCK 0x20
#define FADE_STEP 1        /* FULL/45 per frame, as the patch */
#define MAX_TRACKS 64
#define RING 32768u        /* frames, 0.74 s at 44.1 kHz (128 KiB) */
#define CHUNK 4096u        /* frames per SD read */

enum { FADE_IDLE, FADE_OUT, FADE_IN };

static struct {
  /* Pack */
  char dir[256], base[128];
  uint64_t present;               /* bit N: "<base>-<N>.pcm" exists */
  unsigned tracks;
  bool enabled;
  /* Game-facing state (main thread) */
  bool playing, repeat, msu_music; /* msu_music: current song is an MSU track */
  uint16_t track;
  uint32_t pos;                   /* frames from data start (logical cursor) */
  uint8_t volume, fade, fade_volume;
  unsigned music_pct, sfx_pct;
  /* Mixer */
  uint32_t step, frac;            /* 16.16 source frames per output frame */
  int16_t s0[2], s1[2];
  bool primed;
  uint32_t wait_gen;              /* request being waited for */
  uint32_t len, loop;             /* from the worker's ack */
  bool ready;
} m;

/* ---- Ring and worker ------------------------------------------------------
 * Single producer (worker) / single consumer (mixer). The consumer publishes
 * requests (track, start frame, repeat) by bumping req_gen; the worker
 * answers by resetting the write index to the read index and storing
 * ack_gen with the track's length and loop point. */
static int16_t ring[RING * 2];
static volatile uint32_t ring_r, ring_w;
static volatile uint32_t req_gen, ack_gen;
static volatile uint16_t req_track; static volatile uint32_t req_pos; static volatile bool req_repeat, req_stop;
static volatile uint32_t ack_len, ack_loop; static volatile bool ack_error;
static FILE *wf; static long wf_data; static uint32_t wf_pos, wf_len, wf_loop; static bool wf_repeat, wf_eof;
static uint32_t worker_gen;
#define LOAD(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define STORE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)

static void track_path(char *out, size_t cap, unsigned track) {
  snprintf(out, cap, "%s/%s-%u.pcm", m.dir, m.base, track);
}

static void worker_open(void) {
  if (wf) { fclose(wf); wf = NULL; }
  wf_eof = true; ack_error = false;
  if (LOAD(req_stop)) { STORE(ack_len, 0); STORE(ack_loop, 0); return; }
  char path[512]; track_path(path, sizeof path, req_track);
  wf = fopen(path, "rb");
  uint8_t h[8];
  if (!wf || fread(h, 1, 8, wf) != 8) { if (wf) fclose(wf); wf = NULL; ack_error = true; return; }
  if (!memcmp(h, "MSU1", 4)) { wf_data = 8; wf_loop = h[4] | h[5] << 8 | h[6] << 16 | (uint32_t)h[7] << 24; }
  else { wf_data = 0; wf_loop = 0; }
  fseek(wf, 0, SEEK_END);
  long bytes = ftell(wf) - wf_data;
  wf_len = bytes > 0 ? (uint32_t)(bytes / 4) : 0;
  if (wf_loop >= wf_len) wf_loop = 0;
  wf_repeat = req_repeat;
  wf_pos = req_pos < wf_len ? req_pos : 0;
  fseek(wf, wf_data + (long)wf_pos * 4, SEEK_SET);
  wf_eof = !wf_len;
  STORE(ack_len, wf_len); STORE(ack_loop, wf_loop);
}

/* One service step: answer a new request or fill one chunk. Returns true
 * when it did something (the worker sleeps otherwise). */
static bool worker_step(void) {
  uint32_t g = LOAD(req_gen);
  if (g != worker_gen) {
    worker_gen = g;
    worker_open();
    STORE(ring_w, LOAD(ring_r));
    STORE(ack_gen, g);
    return true;
  }
  if (!wf || wf_eof) return false;
  uint32_t w = ring_w, r = LOAD(ring_r), space = RING - (w - r);
  if (space < CHUNK) return false;
  uint32_t want = CHUNK, at = w % RING;
  if (at + want > RING) want = RING - at;
  if (wf_pos + want > wf_len) want = wf_len - wf_pos;
  size_t got = want ? fread(ring + at * 2, 4, want, wf) : 0;
  if (got < want) memset(ring + (at + got) * 2, 0, (want - got) * 4); /* short file */
  wf_pos += want;
  STORE(ring_w, w + want);
  if (wf_pos >= wf_len) {
    if (wf_repeat) { wf_pos = wf_loop; fseek(wf, wf_data + (long)wf_loop * 4, SEEK_SET); }
    else wf_eof = true;
  }
  return true;
}

#ifdef __3DS__
static Thread worker_thread;
static volatile bool worker_quit;
static LightEvent worker_wake;
static void worker_main(void *arg) {
  (void)arg;
  while (!worker_quit) {
    if (!worker_step()) LightEvent_WaitTimeout(&worker_wake, 8000000LL); /* 8 ms */
  }
  if (wf) { fclose(wf); wf = NULL; }
}
static void worker_kick(void) { LightEvent_Signal(&worker_wake); }
#else
/* Host: synchronous service, so tests are deterministic. */
static void worker_kick(void) { while (worker_step()) {} }
#endif

/* ---- Pack discovery ------------------------------------------------------ */
static bool match_name(const char *name, char *base, size_t cap, unsigned *track) {
  size_t n = strlen(name);
  if (n < 7 || strcasecmp(name + n - 4, ".pcm")) return false;
  size_t i = n - 4;
  if (!isdigit((unsigned char)name[i - 1])) return false;
  while (i > 0 && isdigit((unsigned char)name[i - 1])) --i;
  if (i < 2 || name[i - 1] != '-') return false;
  size_t b = i - 1;
  if (b >= cap) return false;
  memcpy(base, name, b); base[b] = 0;
  *track = (unsigned)atoi(name + i);
  return true;
}

unsigned Mmx3dsMsuInit(const char *dir) {
  memset(&m, 0, sizeof m);
  m.music_pct = 80; m.sfx_pct = 100; m.enabled = true;
  m.step = (uint32_t)(MSU_RATE * 65536.0 / MMX3DS_NATIVE_RATE + 0.5);
  snprintf(m.dir, sizeof m.dir, "%s", dir);
  DIR *d = opendir(dir);
  if (!d) return 0;
  struct { char base[128]; uint64_t bits; unsigned count; } cand[8];
  unsigned nc = 0; struct dirent *e;
  while ((e = readdir(d))) {
    char b[128]; unsigned t;
    if (!match_name(e->d_name, b, sizeof b, &t) || t >= MAX_TRACKS) continue;
    unsigned k = 0;
    while (k < nc && strcmp(cand[k].base, b)) ++k;
    if (k == nc) { if (nc == 8) continue; snprintf(cand[nc].base, sizeof cand[nc].base, "%s", b); cand[nc].bits = 0; cand[nc].count = 0; ++nc; }
    if (!(cand[k].bits & (1ull << t))) { cand[k].bits |= 1ull << t; ++cand[k].count; }
  }
  closedir(d);
  unsigned best = 0;
  for (unsigned k = 1; k < nc; ++k) if (cand[k].count > cand[best].count) best = k;
  if (!nc || !cand[best].count) return 0;
  snprintf(m.base, sizeof m.base, "%s", cand[best].base);
  m.present = cand[best].bits; m.tracks = cand[best].count;
  return m.tracks;
}

unsigned Mmx3dsMsuTracks(void) { return m.tracks; }
const char *Mmx3dsMsuPackName(void) { return m.base; }
void Mmx3dsMsuSetEnabled(bool enabled) { m.enabled = enabled; }
bool Mmx3dsMsuEnabled(void) { return m.tracks && m.enabled; }
bool Mmx3dsMsuPlaying(void) { return m.msu_music && m.playing; }
void Mmx3dsMsuSetGains(unsigned music_pct, unsigned sfx_pct) { m.music_pct = music_pct; m.sfx_pct = sfx_pct; }

static void request(uint16_t track, uint32_t pos, bool repeat, bool stop) {
  req_track = track; req_pos = pos; req_repeat = repeat; req_stop = stop;
  uint32_t g = req_gen + 1;
  STORE(req_gen, g);
  m.wait_gen = g; m.ready = false; m.primed = false; m.frac = 0;
  worker_kick();
}

static bool track_loops(unsigned track) {
  return !(track == 0x00 || track == 0x0f || track == 0x11 || track == 0x12 || track == 0x17 || track == 0x1e);
}

/* ---- Game hooks ----------------------------------------------------------- */
/* Called by the generated code at $80:87B0 (every music command) and at the
 * stage-load JMP ($00:9A2D). Non-zero: consumed, the SPC never hears it. */
int mmx_msu1_music_command(uint8_t command) {
  if (!m.tracks) return 0;
  if (command < 0x10 || command > 0x30) return 0;
  unsigned track = command - 0x10;
#ifndef __3DS__
  if (getenv("MMX_MSU_LOG")) fprintf(stderr, "MSU command %02x track %u %s\n", command, track,
      m.enabled && (m.present & (1ull << track)) ? "msu" : "spc");
#endif
  if (!m.enabled || !(m.present & (1ull << track))) {
    /* The SNES music takes over (missing track or MSU switched off). */
    if (m.msu_music) { m.msu_music = m.playing = false; request(0, 0, false, true); }
    return 0;
  }
  m.track = (uint16_t)track; m.repeat = track_loops(track);
  m.pos = 0; m.playing = true; m.msu_music = true;
  m.volume = VOLUME_FULL; m.fade = FADE_IDLE; m.fade_volume = VOLUME_FULL;
  request(m.track, 0, m.repeat, false);
  return 1;
}

static bool injecting;
static int apu_observer(uint16 reg, uint8 value) {
  if (reg != 0x2140 || !m.tracks || !m.msu_music || injecting) return 0;
  switch (value) {
  case 0xf5: /* resume music: stop the SPC's, fade the MSU track back in */
    injecting = true; RtlApuWrite(0x2140, 0xf6); injecting = false;
    m.playing = true; m.fade = FADE_IN; m.fade_volume = 0; m.volume = 0;
    return 1;
  case 0xf6: /* fade out */
    if (m.playing) { m.fade = FADE_OUT; m.fade_volume = VOLUME_FULL; }
    return 0;
  case 0xfe: m.fade = FADE_IDLE; m.volume = m.fade_volume = VOLUME_FULL; return 0;
  case 0xff: m.fade = FADE_IDLE; m.volume = m.fade_volume = VOLUME_DUCK; return 0;
  default: return 0;
  }
}

/* The stage-load site is often reached through the interpreter tier. Same
 * return shape as the desktop plugin (DarkShock's JMP replacement RTSes). */
static void stage_music_hook(CpuState *cpu, uint32_t pc24) {
  (void)pc24;
  if (mmx_msu1_music_command((uint8_t)(cpu->A & 0xff))) {
    cpu->S = (uint16_t)(cpu->S + 1);
    uint16_t lo = cpu_read8(cpu, 0, cpu->S);
    cpu->S = (uint16_t)(cpu->S + 1);
    uint16_t hi = cpu_read8(cpu, 0, cpu->S);
    uint32_t rpc = ((uint32_t)((hi << 8) | lo) + 1) & 0xffffu;
    interp_bridge_pre_opcode_redirect(((uint32_t)cpu->PB << 16) | rpc);
  }
}

static bool hooks_installed;
void Mmx3dsMsuInstallHooks(void) {
  if (!m.tracks || hooks_installed) return;
  hooks_installed = true;
  RtlAddApuPortObserver(apu_observer);
  interp_bridge_set_pre_opcode_hook(0x809a2d, stage_music_hook);
#ifdef __3DS__
  LightEvent_Init(&worker_wake, RESET_ONESHOT);
  s32 prio = 0x30; svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
  worker_thread = threadCreate(worker_main, NULL, 16 * 1024, prio > 0x19 ? prio - 1 : prio, -2, false);
#endif
}

void Mmx3dsMsuShutdown(void) {
#ifdef __3DS__
  if (worker_thread) { worker_quit = true; worker_kick(); threadJoin(worker_thread, U64_MAX); threadFree(worker_thread); worker_thread = NULL; }
#else
  if (wf) { fclose(wf); wf = NULL; }
#endif
}

void Mmx3dsMsuFrame(void) {
  if (!m.msu_music || m.fade == FADE_IDLE) return;
  if (m.fade == FADE_OUT) {
    if (m.fade_volume <= FADE_STEP) { m.fade_volume = m.volume = 0; m.fade = FADE_IDLE; m.playing = false; return; }
    m.fade_volume -= FADE_STEP;
  } else {
    if (m.fade_volume >= VOLUME_FULL - FADE_STEP) { m.fade_volume = VOLUME_FULL; m.fade = FADE_IDLE; }
    else m.fade_volume += FADE_STEP;
  }
  m.volume = m.fade_volume;
}

/* ---- Mixer ---------------------------------------------------------------- */
static inline int16_t clamp16(int v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

/* Next source frame; false when the ring has nothing yet (silence, the
 * cursor does not move). */
static bool pop(int16_t f[2]) {
  uint32_t r = ring_r;
  if (r == LOAD(ring_w)) { f[0] = f[1] = 0; return false; }
  const int16_t *s = ring + (r % RING) * 2;
  f[0] = s[0]; f[1] = s[1];
  STORE(ring_r, r + 1);
  if (++m.pos >= m.len) {
    if (m.repeat) m.pos = m.loop;
    else { m.playing = false; }
  }
  return true;
}

void Mmx3dsMsuMix(int16_t *pcm, unsigned frames) {
  if (!m.msu_music) return;
#ifndef __3DS__
  worker_kick();
#endif
  if (!m.ready) {
    if (LOAD(ack_gen) != m.wait_gen) goto effects;
    m.ready = true; m.len = LOAD(ack_len); m.loop = LOAD(ack_loop);
    if (ack_error || !m.len) { m.playing = false; m.msu_music = false; return; }
  }
  if (m.playing) {
    if (!m.primed) { pop(m.s0); pop(m.s1); m.primed = true; }
    const int gain = (int)(m.volume * m.music_pct * 32768u / (VOLUME_FULL * 100u));
    const int sfx = (int)(m.sfx_pct * 32768u / 100u);
    for (unsigned i = 0; i < frames && m.playing; ++i) {
      int f = (int)(m.frac >> 1); /* 15-bit fraction */
      for (int c = 0; c < 2; ++c) {
        int music = m.s0[c] + (((m.s1[c] - m.s0[c]) * f) >> 15);
        int fx = (pcm[i * 2 + c] * sfx) >> 15;
        pcm[i * 2 + c] = clamp16(fx + ((music * gain) >> 15));
      }
      m.frac += m.step;
      while (m.frac >= 65536) {
        m.frac -= 65536;
        m.s0[0] = m.s1[0]; m.s0[1] = m.s1[1];
        pop(m.s1);
      }
    }
#ifdef __3DS__
    if (RING - (LOAD(ring_w) - ring_r) >= CHUNK) worker_kick();
#endif
    return;
  }
effects:
  if (m.sfx_pct != 100) {
    const int sfx = (int)(m.sfx_pct * 32768u / 100u);
    for (unsigned i = 0; i < frames * 2; ++i) pcm[i] = clamp16((pcm[i] * sfx) >> 15);
  }
}

/* ---- Savestates ------------------------------------------------------------ */
void Mmx3dsMsuSaveState(uint8_t out[MMX3DS_MSU_STATE_SIZE]) {
  memset(out, 0, MMX3DS_MSU_STATE_SIZE);
  memcpy(out, "MSU3", 4);
  out[4] = m.msu_music; out[5] = m.playing; out[6] = m.repeat; out[7] = m.volume;
  out[8] = m.fade; out[9] = m.fade_volume;
  out[10] = (uint8_t)m.track; out[11] = (uint8_t)(m.track >> 8);
  for (int k = 0; k < 4; ++k) out[12 + k] = (uint8_t)(m.pos >> (8 * k));
}

void Mmx3dsMsuLoadState(const uint8_t *in) {
  if (!m.tracks) return;
  if (!in || memcmp(in, "MSU3", 4) || !in[4]) {
    if (m.msu_music) request(0, 0, false, true);
    m.msu_music = m.playing = false;
    return;
  }
  m.msu_music = true; m.playing = in[5]; m.repeat = in[6]; m.volume = in[7];
  m.fade = in[8]; m.fade_volume = in[9];
  m.track = (uint16_t)(in[10] | in[11] << 8);
  m.pos = in[12] | in[13] << 8 | in[14] << 16 | (uint32_t)in[15] << 24;
  if (!(m.present & (1ull << (m.track & 63)))) { m.msu_music = m.playing = false; return; }
  request(m.track, m.pos, m.repeat, false);
}
