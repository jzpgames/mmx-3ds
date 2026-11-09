#pragma once
/* Opt-in co-op diagnostics (--coop-trace, MMX_COOP_TRACE or logging.ini
 * CoopTrace). Observes only: never writes guest RAM or co-op state, so a
 * traced session simulates exactly like an untraced one. Disabled cost is one
 * predictable branch per hook. */
#include "mmx_coop.h"
#include <stdbool.h>
#include <stdint.h>

extern bool g_mmx_coop_trace;

enum {
  MMX_COOP_TRACE_OFF,
  MMX_COOP_TRACE_ANOMALIES, /* ring buffers dumped around each anomaly */
  MMX_COOP_TRACE_ALL,       /* plus one summary line per frame */
};

/* Hook-boundary events, recorded into a ring and printed only in dumps. */
enum {
  MMX_COOP_EV_SELECT,       /* a=from seat, b=to seat */
  MMX_COOP_EV_CONTROLLER,   /* a=phase: 0 enter, 1 P2 start, 2 P2 end, 3 done */
  MMX_COOP_EV_OBJECT,       /* a=phase: 0 enter, 1 P2 start, 2 P2 end, 4 entry ignored; b=entry low byte */
  MMX_COOP_EV_CONTACT,      /* a=phase: 0 enter, 1 P2 start, 2 P2 end, 3 closed without retry; b=A */
  MMX_COOP_EV_PLATFORM,     /* a=phase: 0 enter, 1 P2 start, 2 end, 4 entry skipped (pass busy); b=rider bits */
  MMX_COOP_EV_PICKUP,       /* a=phase: 0 enter, 1 P2 start, 2 end, 3 collected, 5 owner projected, 6 restore */
  MMX_COOP_EV_SLIME,        /* a=1 owner projected (b=seat), 2 restore */
  MMX_COOP_EV_DOOR,         /* a=door pass */
  MMX_COOP_EV_DEATH,        /* b=seat */
  MMX_COOP_EV_PIT,          /* camera-bottom fatal hit for the partner */
  MMX_COOP_EV_MENU,         /* a=1 open, 0 close */
  MMX_COOP_EV_SCENE,        /* a=1 partner leaves; b=scene owner */
  MMX_COOP_EV_FRAME,        /* a=0 frame tick enter */
  MMX_COOP_EV_COUNT
};

typedef struct MmxCoopTraceEvent {
  uint32_t frame, pc;
  uint16_t s, d;
  uint8_t kind, a, b, current, anchor;
  uint8_t controller_pass, object_pass, contact_pass, pickup_pass, door_pass;
} MmxCoopTraceEvent;

/* Per-frame seat markers: which per-player passes really executed. */
enum {
  MMX_COOP_RAN_CONTROLLER = 1,
  MMX_COOP_RAN_TERRAIN = 2,     /* $81:9D67 post-enemy terrain pass */
  MMX_COOP_RAN_PLATFORM = 4,    /* $84:AB81 moving-platform rider contact */
  MMX_COOP_RAN_NON_ANCHOR = 8,  /* a world pass began with the partner projected */
  MMX_COOP_RAN_PLATFORM_SKIPPED = 16, /* rider contact began while another pass was open */
};

void MmxCoopTraceConfigure(unsigned mode);
unsigned MmxCoopTraceMode(void);
void MmxCoopTraceRecord(const MmxCoopTraceEvent *event);
void MmxCoopTraceMark(unsigned seat, unsigned flags);
void MmxCoopTraceFrameBegin(const MmxCoopState *state);
void MmxCoopTraceFrameEnd(const MmxCoopState *state, const uint8_t ram[0x20000]);
void MmxCoopTraceStateLoaded(void);
/* Host-side, between frames: writes any requested anomaly snapshots. */
void MmxCoopTraceAfterFrame(bool allow_snapshots);
