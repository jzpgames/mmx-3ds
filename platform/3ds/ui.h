#pragma once
/* Bottom-screen interface (revision 13). Pure C, no libctru: it draws into a
 * row-major 320x240 RGB565 canvas and converts it to the LCD's rotated
 * layout, so the host can render it for review. Artwork: ui_assets.c. */
#include <stdbool.h>
#include <stdint.h>
#include "game_core.h"

enum { UI_SLOTS = 3, UI_DETAIL_LINES = 16, UI_LINE = 64 };

typedef enum UiActionKind {
  UI_NONE, UI_SAVE, UI_LOAD, UI_DISPLAY, UI_FILTER, UI_PROFILE, UI_RESET,
  UI_MUSIC_SOURCE, UI_MUSIC_VOL, UI_SFX_VOL /* revision 18; arg: 0/1, -1/+1 */
} UiActionKind;
typedef struct UiAction { UiActionKind kind; int arg; } UiAction;

typedef struct UiStats {
  float fps;            /* images per second on the top screen */
  unsigned depth;       /* 3D slider, percent */
  float game_ms;        /* main-thread time per image (budget 33.4 ms) */
  float stereo_ms;      /* composer time per image */
  unsigned audio_issues;/* underruns + losses in the last second */
  bool polyphase, profile;
} UiStats;

void UiInit(void);
void UiSetSlot(int slot, const Mmx3dsStateInfo *info); /* NULL: empty */
void UiSetSelected(int slot);
int UiSelected(void);
bool UiSlotUsed(int slot);
void UiSetDisplay(int mode);
/* Revision 18: SONIDO card. Volumes in percent (0..150). */
void UiSetAudio(bool pack, bool msu, unsigned music_pct, unsigned sfx_pct);
void UiSetStats(const UiStats *stats);
/* Only shown on the detail page. */
void UiSetDetail(const char lines[UI_DETAIL_LINES][UI_LINE]);
bool UiDetailVisible(void);
void UiToast(const char *text, bool ok, double now_ms);
void UiSetNow(int64_t seconds); /* wall clock for "Hoy HH:MM" */
UiAction UiTouch(int x, int y, double now_ms);
/* Redraws if anything changed; returns true when the canvas changed. */
bool UiRender(double now_ms);
const uint16_t *UiCanvas(void);
/* libctru bottom framebuffer, RGB565, column-major (rotated). */
void UiPresent(uint16_t *framebuffer);
