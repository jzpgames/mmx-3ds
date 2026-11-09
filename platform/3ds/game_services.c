/* Single-threaded host services. The audio device consumes independent PCM
 * buffers and never mutates the SNES APU, so these locks need no mutex. */
#include "common_rtl.h"
#include "mmx_boss_rush_score.h"
#include "mmx_display.h"
#include "mmx_replay.h"
#include "host_report.h"
#include "diagnostics/trace_writer.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

SpcPlayer *g_spc_player;
bool g_new_ppu = true, g_ws_active;
int g_ws_extra;
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}
void MkDir(const char *s) { mkdir(s, 0777); }
void Die(const char *message) {
  fputs(message, stderr); fputc('\n', stderr);
  fflush(stderr); abort();
}
void host_report_breadcrumb(const char *fmt, ...) {
  va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
  fputc('\n', stderr);
}

/* These desktop features are absent, rather than partially emulated. */
void MmxDisplay_SetWidescreenEnabled(bool enabled) { (void)enabled; }
bool MmxDisplay_IsWidescreenEnabled(void) { return false; }
bool MmxDisplay_IsWidescreenActive(void) { return false; }
int MmxDisplay_GetCurrentFrameWidth(void) { return 256; }
void MmxReplayStateLoaded(void) {}
uint32_t MmxBossRushHighScore(void) { return 0; }
bool MmxBossRushScoreOpen(const char *path) { (void)path; return false; }
bool MmxBossRushScoreRecord(uint32_t value) { (void)value; return false; }
/* mmx_msu1_music_command: msu3ds.c (revision 18). */
SnesTraceWriter *SnesTraceWriterOpen(const char *stem, const char *header, size_t bytes, unsigned keep) {
  (void)stem; (void)header; (void)bytes; (void)keep; return NULL;
}
bool SnesTraceWriterWrite(SnesTraceWriter *w, const char *row, size_t size) {
  (void)w; (void)row; (void)size; return false;
}
const char *SnesTraceWriterError(SnesTraceWriter *w) { (void)w; return "Trace writer unavailable in this host"; }
const char *SnesTraceWriterClose(SnesTraceWriter *w) { (void)w; return NULL; }
