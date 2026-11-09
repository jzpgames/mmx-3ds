#include "mmx_coop_trace.h"
#include "common_rtl.h"
#include "host_paths.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#define trace_mkdir(path) _mkdir(path)
#else
#include <sys/stat.h>
#define trace_mkdir(path) mkdir(path, 0755)
#endif

extern int snes_frame_counter;
#ifndef SNESRECOMP_BUILD_VERSION
#define SNESRECOMP_BUILD_VERSION "dev"
#endif

bool g_mmx_coop_trace;
static unsigned mode;
static FILE *out;
static char folder[1024], stamp[32];

/* ~6 seconds of hook events with both players fighting a screen of enemies,
 * and four seconds of per-frame physics. Only the tail is printed per dump. */
enum { EVENTS = 1 << 15, FRAMES = 256, DUMP_FRAMES = 150, DUMP_EVENT_FRAMES = 90,
       MAX_DUMPS = 30, DUMP_COOLDOWN = 120, MAX_SNAPSHOTS = 8, ROLLING_PERIOD = 120 };
static MmxCoopTraceEvent events[EVENTS];
static unsigned event_head, event_count;

typedef struct SeatRecord {
  uint16_t x, y, prev_x, prev_y;
  int16_t vx, vy;
  uint8_t x_sub, y_sub, status, character, action, subaction, ground, external, hp, swap, ran;
} SeatRecord;
typedef struct FrameRecord {
  uint32_t frame;
  uint16_t camera_x, camera_y;
  uint8_t anchor, current, menu, scene, mode_d1, mode_d2, mode_d3, script, riders;
  SeatRecord seat[2];
} FrameRecord;
static FrameRecord frames[FRAMES];
static unsigned frame_head, frame_count;

static unsigned ran[2];
static bool history[2], embedded[2];
static uint16_t last_x[2], last_y[2];
static uint32_t last_frame;
static unsigned anomalies, dumps;
static bool pending_dump;
static uint32_t last_dump = 0xffffffffu;
static unsigned snapshots, pending_snapshot, rolling, rolling_age;
static bool rolling_valid[2];

static unsigned word(const uint8_t *p) { return p[0] | p[1] << 8; }
static bool live_frame(void) { return g_mmx_coop_trace && !RtlSpeculativeFrame(); }

static bool open_folder(const char *path) {
  trace_mkdir(path);
  char probe[1100];
  snprintf(probe, sizeof(probe), "%s/.coop-trace-probe", path);
  FILE *f = fopen(probe, "wb");
  if (!f) return false;
  fclose(f); remove(probe);
  snprintf(folder, sizeof(folder), "%s", path);
  return true;
}
static void open_log(void) {
  if (out) return;
  char path[1100];
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  if (!t || !strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", t)) snprintf(stamp, sizeof(stamp), "session");
  /* Beside the executable when possible (the Windows logs/ folder), else the
   * working directory, which is the save root for portable installs. */
  if (!(snesrecomp_exe_dir_path("logs", path, sizeof(path)) && open_folder(path)) &&
      !open_folder("logs")) snprintf(folder, sizeof(folder), ".");
  snprintf(path, sizeof(path), "%s/mmx-coop-trace-%s.log", folder, stamp);
  out = fopen(path, "w");
  if (!out) { fprintf(stderr, "[mmx-coop-trace] cannot open %s\n", path); g_mmx_coop_trace = false; return; }
  fprintf(stderr, "[mmx-coop-trace] writing %s\n", path);
  fprintf(out, "# Mega Man X co-op trace, %s mode, build %s\n",
      mode == MMX_COOP_TRACE_ALL ? "all-frames" : "anomaly", SNESRECOMP_BUILD_VERSION);
  fprintf(out, "# Each ANOMALY line is followed by the recent per-frame physics and hook events.\n"
               "# Seat fields: x/y = position.subpixel, px/py = native previous position ($0BCA/$0BCC),\n"
               "# vx/vy = $0BC2/$0BC4, act = $0BAA/$0BAB, gnd = $0BD3, ext = $0BD4,\n"
               "# ran = passes this frame: C controller, T terrain $81:9D67, P platform $84:AB81,\n"
               "#       ! world pass began with the partner projected, s platform retry skipped.\n");
  fflush(out);
}

void MmxCoopTraceConfigure(unsigned value) {
  mode = value > MMX_COOP_TRACE_ALL ? MMX_COOP_TRACE_ALL : value;
  g_mmx_coop_trace = mode != MMX_COOP_TRACE_OFF;
  if (g_mmx_coop_trace) open_log();
}
unsigned MmxCoopTraceMode(void) { return g_mmx_coop_trace ? mode : MMX_COOP_TRACE_OFF; }

void MmxCoopTraceRecord(const MmxCoopTraceEvent *event) {
  if (!live_frame()) return;
  events[event_head] = *event;
  event_head = (event_head + 1) % EVENTS;
  if (event_count < EVENTS) ++event_count;
}
void MmxCoopTraceMark(unsigned seat, unsigned flags) {
  if (live_frame() && seat < 2) ran[seat] |= flags;
}

static const char *const kinds[MMX_COOP_EV_COUNT] = {
  "select", "controller", "object", "contact", "platform", "pickup", "slime",
  "door", "death", "pit", "menu", "scene", "frame"};
static void print_event(const MmxCoopTraceEvent *e) {
  fprintf(out, "  ev f=%u %-10s pc=%06X a=%02X b=%02X cur=%u anc=%u S=%04X D=%04X pass[c%u o%u k%u p%u d%u]\n",
      e->frame, e->kind < MMX_COOP_EV_COUNT ? kinds[e->kind] : "?", e->pc, e->a, e->b,
      e->current, e->anchor, e->s, e->d, e->controller_pass, e->object_pass,
      e->contact_pass, e->pickup_pass, e->door_pass);
}
static void print_seat(unsigned i, const SeatRecord *s) {
  static const char *const status[] = {"absent", "alive", "fallen"};
  fprintf(out, " P%u %s %-6s x=%04X.%02X y=%04X.%02X px=%04X py=%04X vx=%+6d vy=%+6d act=%02X/%02X gnd=%02X ext=%02X hp=%2u sw=%u ran=%c%c%c%c%c",
      i + 1, s->character ? "Zero" : "X   ", s->status < 3 ? status[s->status] : "?",
      s->x, s->x_sub, s->y, s->y_sub, s->prev_x, s->prev_y, s->vx, s->vy,
      s->action, s->subaction, s->ground, s->external, s->hp, s->swap,
      s->ran & MMX_COOP_RAN_CONTROLLER ? 'C' : '-', s->ran & MMX_COOP_RAN_TERRAIN ? 'T' : '-',
      s->ran & MMX_COOP_RAN_PLATFORM ? 'P' : '-', s->ran & MMX_COOP_RAN_NON_ANCHOR ? '!' : '-',
      s->ran & MMX_COOP_RAN_PLATFORM_SKIPPED ? 's' : '-');
}
static void print_frame(const FrameRecord *f) {
  fprintf(out, "  fr f=%u anc=%u cur=%u mode=%02X/%02X/%02X cam=%04X,%04X menu=%u scene=%u script=%02X riders=%u |",
      f->frame, f->anchor, f->current, f->mode_d1, f->mode_d2, f->mode_d3,
      f->camera_x, f->camera_y, f->menu, f->scene, f->script, f->riders);
  print_seat(0, &f->seat[0]); fputs(" |", out); print_seat(1, &f->seat[1]); fputc('\n', out);
}

static void dump(uint32_t frame) {
  unsigned n = frame_count < DUMP_FRAMES ? frame_count : DUMP_FRAMES;
  fprintf(out, "--- last %u frames ---\n", n);
  for (unsigned i = 0; i < n; ++i)
    print_frame(&frames[(frame_head + FRAMES - n + i) % FRAMES]);
  unsigned first = event_count;
  while (first && events[(event_head + EVENTS - first) % EVENTS].frame + DUMP_EVENT_FRAMES < frame) --first;
  fprintf(out, "--- hook events from the last %u frames (%u) ---\n", DUMP_EVENT_FRAMES, first);
  for (unsigned i = first; i; --i) print_event(&events[(event_head + EVENTS - i) % EVENTS]);
  fputs("--- end ---\n", out);
}
static void anomaly(uint32_t frame, const char *format, ...) {
  if (!out) return;
  va_list args;
  ++anomalies;
  fprintf(out, "ANOMALY #%u f=%u: ", anomalies, frame);
  va_start(args, format); vfprintf(out, format, args); va_end(args);
  fputc('\n', out);
  fprintf(stderr, "[mmx-coop-trace] anomaly #%u at frame %u\n", anomalies, frame);
  /* Several detectors usually fire for one fault; print its context once. */
  if (last_dump != 0xffffffffu && frame - last_dump < DUMP_COOLDOWN) {
    fputs("  (context already dumped for a recent anomaly)\n", out);
  } else if (dumps >= MAX_DUMPS) {
    fputs("  (dump limit reached; summaries only)\n", out);
  } else {
    ++dumps; last_dump = frame; pending_dump = true;
    /* Printed by the next FrameEnd, after this frame's record is stored. */
    if (snapshots < MAX_SNAPSHOTS && !pending_snapshot) pending_snapshot = anomalies;
  }
  fflush(out);
}

void MmxCoopTraceFrameBegin(const MmxCoopState *s) {
  if (!live_frame() || !s) return;
  ran[0] = ran[1] = 0;
  if (s->controller_pass || s->object_pass || s->contact_pass || s->pickup_pass || s->door_pass)
    anomaly((uint32_t)snes_frame_counter,
        "co-op pass still open at frame start: controller=%u object=%u(entry %04X) contact=%u(entry %04X S=%04X D=%04X) pickup=%u door=%u",
        s->controller_pass, s->object_pass, s->object_entry, s->contact_pass, s->contact_entry,
        s->contact_s, s->contact_d, s->pickup_pass, s->door_pass);
}

static bool flat_floor(unsigned type) {
  return type == 0x13 || (type >= 0x34 && type <= 0x38) || (type >= 0x3b && type <= 0x3d);
}
static bool one_way(unsigned type) { return type == 0x39 || type == 0x3a; }

static void record_seat(SeatRecord *s, const MmxCoopPlayer *p, unsigned seat) {
  const uint8_t *b = p->body;
  s->x = (uint16_t)word(b + 5); s->x_sub = b[4];
  s->y = (uint16_t)word(b + 8); s->y_sub = b[7];
  s->prev_x = (uint16_t)word(b + 0x22); s->prev_y = (uint16_t)word(b + 0x24);
  s->vx = (int16_t)word(b + 0x1a); s->vy = (int16_t)word(b + 0x1c);
  s->action = b[2]; s->subaction = b[3]; s->ground = b[0x2b]; s->external = b[0x2c];
  s->hp = b[0x27] & 127; s->status = p->status; s->character = p->character;
  s->swap = p->zero.swap_phase; s->ran = (uint8_t)ran[seat];
}

void MmxCoopTraceFrameEnd(const MmxCoopState *s, const uint8_t r[0x20000]) {
  if (!live_frame() || !s || !r || !out) return;
  uint32_t frame = (uint32_t)snes_frame_counter;
  if (frame != last_frame + 1) history[0] = history[1] = false;
  last_frame = frame;
  FrameRecord *f = &frames[frame_head];
  memset(f, 0, sizeof(*f));
  f->frame = frame; f->anchor = s->anchor; f->current = s->current;
  f->menu = s->menu_owner; f->scene = s->scene_phase;
  f->mode_d1 = r[0xd1]; f->mode_d2 = r[0xd2]; f->mode_d3 = r[0xd3]; f->script = r[0x1f0c];
  f->camera_x = (uint16_t)word(r + 0x1e4d); f->camera_y = (uint16_t)word(r + 0x1e50);
  f->riders = s->platform_riders;
  for (unsigned i = 0; i < 2; ++i) record_seat(&f->seat[i], &s->players[i], i);
  frame_head = (frame_head + 1) % FRAMES;
  if (frame_count < FRAMES) ++frame_count;
  if (mode == MMX_COOP_TRACE_ALL) print_frame(f);

  bool gameplay = s->initialized && r[0xd1] == 2 && r[0xd2] == 4 && r[0xd3] == 4 &&
      !r[0x1f0c] && !s->menu_owner && !s->scene_owner;
  bool pair = gameplay && s->players[0].status == MMX_COOP_ALIVE && s->players[1].status == MMX_COOP_ALIVE &&
      !s->players[0].zero.swap_phase && !s->players[1].zero.swap_phase &&
      (s->players[0].body[0x27] & 127) && (s->players[1].body[0x27] & 127);
  if (gameplay && s->current != s->anchor)
    anomaly(frame, "frame ended with P%u projected while P%u is the world anchor", s->current + 1, s->anchor + 1);
  if ((ran[0] | ran[1]) & MMX_COOP_RAN_NON_ANCHOR)
    anomaly(frame, "a world pass ($81:9D67 terrain or $81:8136 controller) began with the partner projected; ran P1=%X P2=%X", ran[0], ran[1]);
  if ((ran[0] | ran[1]) & MMX_COOP_RAN_PLATFORM_SKIPPED)
    anomaly(frame, "moving-platform rider contact ($84:AB81/AB56) skipped its second-seat pass because another co-op pass was still open");
  if (pair) {
    unsigned differ = (ran[0] ^ ran[1]) &
        (MMX_COOP_RAN_CONTROLLER | MMX_COOP_RAN_TERRAIN | MMX_COOP_RAN_PLATFORM);
    if (differ)
      anomaly(frame, "per-player pass ran for only one seat: %s%s%s(P1=%c%c%c P2=%c%c%c)",
          differ & MMX_COOP_RAN_CONTROLLER ? "controller " : "", differ & MMX_COOP_RAN_TERRAIN ? "terrain " : "",
          differ & MMX_COOP_RAN_PLATFORM ? "platform " : "",
          ran[0] & 1 ? 'C' : '-', ran[0] & 2 ? 'T' : '-', ran[0] & 4 ? 'P' : '-',
          ran[1] & 1 ? 'C' : '-', ran[1] & 2 ? 'T' : '-', ran[1] & 4 ? 'P' : '-');
  }
  for (unsigned i = 0; i < 2; ++i) {
    const MmxCoopPlayer *p = &s->players[i];
    const uint8_t *b = p->body;
    bool active = gameplay && p->status == MMX_COOP_ALIVE && !p->zero.swap_phase &&
        (b[0x27] & 127) && b[2] != 12;
    int x = (int)word(b + 5), y = (int)word(b + 8), feet = y + 16;
    if (!active) { history[i] = embedded[i] = false; continue; }
    if (history[i]) {
      int previous = last_y[i] + 16;
      if (feet - previous > 12)
        anomaly(frame, "P%u dropped %d px in one frame (y %04X -> %04X, vy=%d)",
            i + 1, feet - previous, last_y[i], y, (int16_t)word(b + 0x1c));
      /* Native terrain resolution never leaves feet below a floor/one-way top
       * that was at or below the feet on the preceding frame. */
      for (int row = previous; row <= feet - 3; ++row) {
        unsigned type = MmxWeaponsTerrainClass(r, x, row);
        int surface = 0;
        if ((flat_floor(type) || one_way(type)) && MmxWeaponsTerrainSolid(r, x, row, true, &surface) &&
            surface >= previous && surface <= feet - 3) {
          anomaly(frame, "P%u passed through a %s top (class %02X) at y=%04X: feet %04X -> %04X, act=%02X/%02X gnd=%02X ext=%02X",
              i + 1, one_way(type) ? "one-way" : "solid floor", type, surface, previous, feet,
              b[2], b[3], b[0x2b], b[0x2c]);
          break;
        }
      }
    }
    int surface = 0;
    unsigned type = MmxWeaponsTerrainClass(r, x, feet - 6);
    bool inside = flat_floor(type) && MmxWeaponsTerrainSolid(r, x, feet - 6, true, &surface) && surface <= feet - 6;
    if (inside && !embedded[i])
      anomaly(frame, "P%u feet are %d px inside solid ground (class %02X, surface %04X, feet %04X), act=%02X/%02X gnd=%02X",
          i + 1, feet - surface, type, surface, feet, b[2], b[3], b[0x2b]);
    embedded[i] = inside;
    history[i] = true; last_x[i] = (uint16_t)x; last_y[i] = (uint16_t)y;
  }
  if (pending_dump) { pending_dump = false; dump(frame); }
  if (gameplay && ++rolling_age >= ROLLING_PERIOD) rolling_age = ROLLING_PERIOD;
  if (mode == MMX_COOP_TRACE_ALL) fflush(out);
}

void MmxCoopTraceStateLoaded(void) {
  if (!g_mmx_coop_trace || !out) return;
  history[0] = history[1] = embedded[0] = embedded[1] = false;
  fprintf(out, "# state loaded at frame %d\n", snes_frame_counter);
  fflush(out);
}

static void snapshot_path(char *path, size_t size, const char *leaf) {
  snprintf(path, size, "%s/mmx-coop-trace-%s-%s.sav", folder, stamp, leaf);
}
static bool copy_file(const char *from, const char *to) {
  FILE *in = fopen(from, "rb"), *dst = in ? fopen(to, "wb") : NULL;
  char buffer[65536];
  size_t n;
  bool ok = in && dst;
  while (ok && (n = fread(buffer, 1, sizeof(buffer), in)) != 0) ok = fwrite(buffer, 1, n, dst) == n;
  if (in) fclose(in);
  if (dst && fclose(dst)) ok = false;
  return ok;
}
void MmxCoopTraceAfterFrame(bool allow) {
  if (!g_mmx_coop_trace || !out || !allow) return;
  char path[1200], leaf[64];
  if (pending_snapshot) {
    /* Two rolling snapshots bracket the 2-4 seconds before the anomaly, so
     * the lead-up can be replayed; "after" shows the resulting state. */
    for (unsigned k = 0; k < 2; ++k) {
      unsigned slot = (rolling + k) % 2; /* older first */
      if (!rolling_valid[slot]) continue;
      char from[1200];
      snprintf(leaf, sizeof(leaf), "rolling-%u", slot); snapshot_path(from, sizeof(from), leaf);
      snprintf(leaf, sizeof(leaf), "anomaly%03u-before%u", pending_snapshot, k + 1); snapshot_path(path, sizeof(path), leaf);
      if (copy_file(from, path)) fprintf(out, "# snapshot %s\n", path);
    }
    snprintf(leaf, sizeof(leaf), "anomaly%03u-after", pending_snapshot); snapshot_path(path, sizeof(path), leaf);
    if (RtlSaveSnapshot(path)) fprintf(out, "# snapshot %s\n", path);
    ++snapshots; pending_snapshot = 0; fflush(out);
  }
  if (rolling_age >= ROLLING_PERIOD) {
    snprintf(leaf, sizeof(leaf), "rolling-%u", rolling); snapshot_path(path, sizeof(path), leaf);
    rolling_valid[rolling] = RtlSaveSnapshot(path);
    rolling ^= 1; rolling_age = 0;
  }
}
