#include "ui.h"
#include "ui_assets.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

enum { W = 320, H = 240 };
static uint16_t canvas[W * H];
static bool dirty = true, detail;
static Mmx3dsStateInfo slots[UI_SLOTS];
static int selected, display;
static UiStats stats;
static char detail_lines[UI_DETAIL_LINES][UI_LINE];
static char toast_text[48];
static bool toast_ok;
static double toast_until, hot_until;
static int hot_slot = -1; static bool hot_load;
static int64_t now_s;
static bool audio_pack, audio_msu = true; static unsigned music_vol = 80, sfx_vol = 100;

/* ---- colours (RGB565) ---------------------------------------------------- */
#define RGB(r, g, b) (uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3))
#define C_TEXT RGB(0xe8, 0xf2, 0xff)
#define C_DIM RGB(0x8e, 0xa6, 0xc8)
#define C_CYAN RGB(0x5f, 0xd0, 0xff)
#define C_CYAN2 RGB(0x9b, 0xe3, 0xff)
#define C_OK RGB(0x59, 0xe3, 0x9a)
#define C_WARN RGB(0xff, 0xb5, 0x4d)
#define C_BAD RGB(0xff, 0x6b, 0x6b)

/* ---- primitives ---------------------------------------------------------- */
static void blit_part(const UiImage *im, int x, int y, int cols) {
  if (cols > im->w) cols = im->w;
  for (int r = 0; r < im->h; ++r) {
    int yy = y + r; if (yy < 0 || yy >= H) continue;
    for (int c = 0; c < cols; ++c) {
      int xx = x + c; if (xx < 0 || xx >= W) continue;
      canvas[yy * W + xx] = im->px[r * im->w + c];
    }
  }
}
static void blit_at(const UiImage *im, int x, int y) { blit_part(im, x, y, im->w); }
static void blit(const UiImage *im) { blit_at(im, im->x, im->y); }

static uint16_t mix(uint16_t d, uint16_t s, unsigned a) {
  unsigned dr = d >> 11, dg = (d >> 5) & 63, db = d & 31;
  unsigned sr = s >> 11, sg = (s >> 5) & 63, sb = s & 31;
  dr += ((int)(sr - dr) * (int)a) / 255; dg += ((int)(sg - dg) * (int)a) / 255;
  db += ((int)(sb - db) * (int)a) / 255;
  return (uint16_t)(dr << 11 | dg << 5 | db);
}

static unsigned next_code(const char **s) {
  const unsigned char *p = (const unsigned char *)*s;
  unsigned c = *p++;
  if (c >= 0xf0) { c = (c & 7) << 18 | (p[0] & 63) << 12 | (p[1] & 63) << 6 | (p[2] & 63); p += 3; }
  else if (c >= 0xe0) { c = (c & 15) << 12 | (p[0] & 63) << 6 | (p[1] & 63); p += 2; }
  else if (c >= 0xc0) { c = (c & 31) << 6 | (p[0] & 63); p += 1; }
  *s = (const char *)p;
  return c;
}
static const UiGlyph *glyph(const UiFont *f, unsigned code) {
  for (unsigned i = 0; i < f->count; ++i) if (f->glyphs[i].code == code) return &f->glyphs[i];
  return code == '?' ? &f->glyphs[0] : glyph(f, '?');
}
static int text_width(const UiFont *f, const char *s) {
  unsigned adv = 0;
  while (*s) adv += glyph(f, next_code(&s))->adv64;
  return (int)((adv + 32) / 64);
}
static int text(const UiFont *f, int x, int y, uint16_t color, const char *s) {
  unsigned pen = (unsigned)x * 64;
  while (*s) {
    const UiGlyph *g = glyph(f, next_code(&s));
    int gx = (int)((pen + 32) / 64) - 3;
    for (int r = 0; r < f->h; ++r) {
      int yy = y + r; if (yy < 0 || yy >= H) continue;
      const uint8_t *a = f->alpha + g->offset + r * g->w;
      for (int c = 0; c < g->w; ++c) {
        int xx = gx + c;
        if (a[c] && xx >= 0 && xx < W) canvas[yy * W + xx] = mix(canvas[yy * W + xx], color, a[c]);
      }
    }
    pen += g->adv64;
  }
  return (int)((pen + 32) / 64);
}
static void text_center(const UiFont *f, UiRect r, int dy, uint16_t color, const char *s) {
  text(f, r.x + (r.w - text_width(f, s)) / 2, r.y + dy, color, s);
}
static void text_right(const UiFont *f, UiRect r, int dy, uint16_t color, const char *s) {
  text(f, r.x + r.w - text_width(f, s), r.y + dy, color, s);
}

/* ---- content ------------------------------------------------------------- */
static const char *const stage_long[] = {
  "Autopista", "Launch Octopus", "Sting Chameleon", "Armored Armadillo",
  "Flame Mammoth", "Storm Eagle", "Spark Mandrill", "Boomer Kuwanger",
  "Chill Penguin", "Sigma 1", "Sigma 2", "Sigma 3", "Sigma 4"};
static const char *const stage_short[] = {
  "Autopista", "Octopus", "Chameleon", "Armadillo", "Mammoth", "Eagle",
  "Mandrill", "Kuwanger", "Penguin", "Sigma 1", "Sigma 2", "Sigma 3", "Sigma 4"};

static void format_time(int64_t t, char *out, size_t n) {
  time_t a = (time_t)t, b = (time_t)now_s;
  struct tm ta, tb; gmtime_r(&a, &ta); gmtime_r(&b, &tb);
  if (now_s && ta.tm_year == tb.tm_year && ta.tm_yday == tb.tm_yday)
    snprintf(out, n, "Hoy %02d:%02d", ta.tm_hour, ta.tm_min);
  else
    snprintf(out, n, "%02d/%02d/%02d %02d:%02d", ta.tm_mday, ta.tm_mon + 1,
             ta.tm_year % 100, ta.tm_hour, ta.tm_min);
}

static void draw_thumb(const uint16_t *px, UiRect r) {
  for (int y = 0; y < r.h && y < MMX3DS_THUMB_H; ++y)
    for (int x = 0; x < r.w && x < MMX3DS_THUMB_W; ++x) {
      /* Leave the 2 px rounded corners to the card background. */
      bool corner = (x == 0 || x == r.w - 1) && (y == 0 || y == r.h - 1);
      if (!corner) canvas[(r.y + y) * W + r.x + x] = px[y * MMX3DS_THUMB_W + x];
    }
}

static void draw_header(double now) {
  (void)now;
  bool good = stats.fps >= 29.0f;
  blit(good ? &ui_chip_ok : &ui_chip_warn);
  char s[24];
  snprintf(s, sizeof s, "%.0f FPS", stats.fps);
  text_center(&ui_font_chip, UI_R_CHIPF, 1, good ? C_OK : C_WARN, s);
  blit(&ui_chip_cyan);
  if (stats.depth) snprintf(s, sizeof s, "3D %u%%", stats.depth);
  else snprintf(s, sizeof s, "3D OFF");
  text_center(&ui_font_chip, UI_R_CHIP3, 1, C_CYAN2, s);
}

static void draw_slot(int i, double now) {
  static const UiImage *const cards[] = {&ui_card0, &ui_card1, &ui_card2};
  static const UiImage *const sels[] = {&ui_card0_sel, &ui_card1_sel, &ui_card2_sel};
  const UiRect names[] = {UI_R_N0, UI_R_N1, UI_R_N2}, thumbs[] = {UI_R_T0, UI_R_T1, UI_R_T2};
  const UiRect metas[] = {UI_R_M0, UI_R_M1, UI_R_M2}, saves[] = {UI_R_G0, UI_R_G1, UI_R_G2};
  const UiRect loads[] = {UI_R_C0, UI_R_C1, UI_R_C2};
  blit(i == selected ? sels[i] : cards[i]);
  const Mmx3dsStateInfo *s = &slots[i];
  char num[4]; snprintf(num, sizeof num, "%d ", i + 1);
  int x = text(&ui_font_name, names[i].x, names[i].y, C_CYAN, num);
  int room = names[i].x + names[i].w - x;
  const char *name = "Vacía";
  if (s->used) {
    if (s->stage < sizeof stage_long / sizeof *stage_long) {
      unsigned k = s->stage;
      name = text_width(&ui_font_name, stage_long[k]) <= room ? stage_long[k] : stage_short[k];
    } else name = "Inicio"; /* title, intro text or menus */
  }
  text(&ui_font_name, x, names[i].y, s->used ? C_TEXT : C_DIM, name);
  if (s->used) draw_thumb(s->thumb, thumbs[i]);
  else blit_at(&ui_thumb_empty, thumbs[i].x, thumbs[i].y);
  char meta[32] = "—";
  if (s->used) format_time(s->time, meta, sizeof meta);
  text(&ui_font_meta, metas[i].x, metas[i].y, C_DIM, meta);
  bool hot = hot_slot == i && now < hot_until;
  blit_at(hot && !hot_load ? &ui_btn_save_hot : &ui_btn_save, saves[i].x, saves[i].y);
  blit_at(!s->used ? &ui_btn_load_off : hot && hot_load ? &ui_btn_load_hot : &ui_btn_load,
          loads[i].x, loads[i].y);
}

static void mini_bar(UiRect r, float fraction, const UiImage *im) {
  if (fraction < 0) fraction = 0;
  if (fraction > 1) fraction = 1;
  int cols = (int)(fraction * r.w + 0.5f);
  if (cols) blit_part(im, r.x, r.y, cols);
}

static void draw_main(double now) {
  memcpy(canvas, ui_bg_main.px, sizeof canvas);
  draw_header(now);
  for (int i = 0; i < UI_SLOTS; ++i) draw_slot(i, now);
  static const UiImage *const segs[] = {&ui_seg0, &ui_seg1, &ui_seg2};
  blit(segs[display]);
  char s[32];
  /* SONIDO: the volumes act on the MSU-1 mix only; dim them otherwise. */
  bool msu = audio_pack && audio_msu;
  blit(!audio_pack ? &ui_aseg_none : msu ? &ui_aseg_msu : &ui_aseg_snes);
  mini_bar(UI_R_VMB, music_vol / 150.0f, &ui_vol_full);
  mini_bar(UI_R_VEB, sfx_vol / 150.0f, &ui_vol_full);
  if (!msu) {
    UiRect r = {UI_R_VM0.x - 30, UI_R_VM0.y, UI_R_VE1.x + UI_R_VE1.w - UI_R_VM0.x + 30, UI_R_VE1.y + UI_R_VE1.h - UI_R_VM0.y};
    for (int y = r.y; y < r.y + r.h; ++y) for (int x = r.x; x < r.x + r.w; ++x)
      canvas[y * W + x] = mix(canvas[y * W + x], RGB(0x13, 0x21, 0x3b), 150);
  }
  const float budget = 1000.0f / 29.913f;
  float g = stats.game_ms / budget, t = stats.stereo_ms / budget;
  mini_bar(UI_R_PB0, g, g < 0.85f ? &ui_mini_ok : g < 1 ? &ui_mini_warn : &ui_mini_bad);
  mini_bar(UI_R_PB1, t, t < 0.85f ? &ui_mini_cyan : t < 1 ? &ui_mini_warn : &ui_mini_bad);
  mini_bar(UI_R_PB2, 1, stats.audio_issues ? &ui_mini_warn : &ui_mini_ok);
  snprintf(s, sizeof s, "%.1f ms", stats.game_ms); for (char *p = s; *p; ++p) if (*p == '.') *p = ',';
  text_right(&ui_font_metab, UI_R_PV0, 0, g < 1 ? C_TEXT : C_BAD, s);
  snprintf(s, sizeof s, "%.1f ms", stats.stereo_ms); for (char *p = s; *p; ++p) if (*p == '.') *p = ',';
  text_right(&ui_font_metab, UI_R_PV1, 0, t < 1 ? C_TEXT : C_BAD, s);
  if (stats.audio_issues) snprintf(s, sizeof s, "%u cortes", stats.audio_issues);
  else snprintf(s, sizeof s, "OK");
  text_right(&ui_font_metab, UI_R_PV2, 0, stats.audio_issues ? C_WARN : C_OK, s);
  if (now < toast_until) {
    blit(toast_ok ? &ui_toast_ok : &ui_toast_err);
    text_center(&ui_font_metab, UI_R_TOAST, 0, toast_ok ? C_OK : C_BAD, toast_text);
  }
}

static void draw_detail(double now) {
  memcpy(canvas, ui_bg_detail.px, sizeof canvas);
  draw_header(now);
  UiRect c = UI_R_DCARD;
  for (int i = 0; i < UI_DETAIL_LINES; ++i)
    text(&ui_font_meta, c.x + 6, c.y + 4 + i * 9, i ? C_TEXT : C_CYAN2, detail_lines[i]);
  text_center(&ui_font_btn, UI_R_BF, 6, C_CYAN2, stats.polyphase ? "Filtro: polifase" : "Filtro: lineal");
  text_center(&ui_font_btn, UI_R_BP, 6, C_CYAN2, stats.profile ? "Desglose: SÍ" : "Desglose: NO");
  text_center(&ui_font_btn, UI_R_BR, 6, C_CYAN2, "Reiniciar");
  text_center(&ui_font_btn, UI_R_BK, 6, C_TEXT, "VOLVER");
  if (now < toast_until) {
    blit(toast_ok ? &ui_toast_ok : &ui_toast_err);
    text_center(&ui_font_metab, UI_R_TOAST, 0, toast_ok ? C_OK : C_BAD, toast_text);
  }
}

/* ---- state --------------------------------------------------------------- */
void UiInit(void) { memset(slots, 0, sizeof slots); dirty = true; }
void UiSetSlot(int i, const Mmx3dsStateInfo *info) {
  if (i < 0 || i >= UI_SLOTS) return;
  if (info) slots[i] = *info; else memset(&slots[i], 0, sizeof slots[i]);
  dirty = true;
}
void UiSetSelected(int i) { if (i >= 0 && i < UI_SLOTS && i != selected) { selected = i; dirty = true; } }
int UiSelected(void) { return selected; }
bool UiSlotUsed(int i) { return i >= 0 && i < UI_SLOTS && slots[i].used; }
void UiSetDisplay(int mode) { if (mode != display) { display = mode; dirty = true; } }
void UiSetStats(const UiStats *s) { stats = *s; dirty = true; }
void UiSetAudio(bool pack, bool msu, unsigned music_pct, unsigned sfx_pct) {
  audio_pack = pack; audio_msu = msu; music_vol = music_pct; sfx_vol = sfx_pct; dirty = true;
}
void UiSetDetail(const char lines[UI_DETAIL_LINES][UI_LINE]) {
  memcpy(detail_lines, lines, sizeof detail_lines);
  if (detail) dirty = true;
}
bool UiDetailVisible(void) { return detail; }
void UiSetNow(int64_t s) { now_s = s; }
void UiToast(const char *t, bool ok, double now) {
  snprintf(toast_text, sizeof toast_text, "%s", t);
  toast_ok = ok; toast_until = now + 2200; dirty = true;
}

static bool inside(UiRect r, int x, int y) {
  return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}
UiAction UiTouch(int x, int y, double now) {
  UiAction a = {UI_NONE, 0};
  dirty = true;
  if (detail) {
    if (y < 24 || inside(UI_R_BK, x, y)) detail = false;
    else if (inside(UI_R_BF, x, y)) a.kind = UI_FILTER;
    else if (inside(UI_R_BP, x, y)) a.kind = UI_PROFILE;
    else if (inside(UI_R_BR, x, y)) a.kind = UI_RESET;
    return a;
  }
  const UiRect cards[] = {UI_R_SLOT0, UI_R_SLOT1, UI_R_SLOT2};
  const UiRect saves[] = {UI_R_G0, UI_R_G1, UI_R_G2}, loads[] = {UI_R_C0, UI_R_C1, UI_R_C2};
  const UiRect segs[] = {UI_R_S0, UI_R_S1, UI_R_S2};
  for (int i = 0; i < UI_SLOTS; ++i) {
    /* Buttons get a 2 px margin: they are small for a finger. */
    UiRect s = saves[i], l = loads[i];
    s.x -= 2; s.y -= 2; s.w += 3; s.h += 4; l.x -= 1; l.y -= 2; l.w += 3; l.h += 4;
    if (inside(s, x, y)) { selected = i; hot_slot = i; hot_load = false; hot_until = now + 250; return (UiAction){UI_SAVE, i}; }
    if (inside(l, x, y)) {
      selected = i;
      if (!slots[i].used) return a;
      hot_slot = i; hot_load = true; hot_until = now + 250; return (UiAction){UI_LOAD, i};
    }
    if (inside(cards[i], x, y)) { selected = i; return a; }
  }
  for (int i = 0; i < 3; ++i)
    if (inside(segs[i], x, y)) { display = i; return (UiAction){UI_DISPLAY, i}; }
  /* SONIDO card; generous margins around the small buttons. */
  { UiRect a0 = UI_R_A0, a1 = UI_R_A1; a0.y -= 3; a0.h += 6; a1.y -= 3; a1.h += 6;
    if (inside(a0, x, y)) return (UiAction){UI_MUSIC_SOURCE, 0};
    if (inside(a1, x, y)) return audio_pack ? (UiAction){UI_MUSIC_SOURCE, 1} : a;
    const UiRect v[4] = {UI_R_VM0, UI_R_VM1, UI_R_VE0, UI_R_VE1};
    for (int i = 0; i < 4; ++i) { UiRect r = v[i]; r.x -= 4; r.y -= 2; r.w += 8; r.h += 3;
      if (inside(r, x, y)) return (UiAction){i < 2 ? UI_MUSIC_VOL : UI_SFX_VOL, (i & 1) ? 1 : -1}; } }
  if (inside(UI_R_PERF, x, y)) detail = true;
  return a;
}

bool UiRender(double now) {
  static bool toast_shown, hot_shown;
  bool t = now < toast_until, h = now < hot_until;
  if (t != toast_shown || h != hot_shown) dirty = true;
  if (!dirty) return false;
  toast_shown = t; hot_shown = h; dirty = false;
  if (detail) draw_detail(now); else draw_main(now);
  return true;
}
const uint16_t *UiCanvas(void) { return canvas; }
void UiPresent(uint16_t *fb) {
  /* Column x of the LCD buffer holds 240 pixels, bottom row first. Walk the
   * canvas in 8-row bands so reads stay within a few cache lines. */
  for (int y0 = 0; y0 < H; y0 += 8)
    for (int x = 0; x < W; ++x) {
      uint16_t *col = fb + x * 240 + 239 - y0;
      const uint16_t *src = canvas + y0 * W + x;
      for (int k = 0; k < 8; ++k) col[-k] = src[k * W];
    }
}
