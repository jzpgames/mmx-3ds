/* Probe ONLY. Optional PC modes stay disabled in this data-free renderer
 * exercise. A future game executable must use the real mode implementations,
 * not silently link these placeholders. */
#include "mmx_boss_rush_score.h"
#include "mmx_knc_bugfix.h"

bool MmxKncBugfixActive(unsigned seat) { (void)seat; return false; }
unsigned MmxKncBugfixPhase(void) { return 0; }
uint32_t MmxBossRushHighScore(void) { return 0; }
bool MmxBossRushScoreOpen(const char *path) { (void)path; return false; }
bool MmxBossRushScoreRecord(uint32_t value) { (void)value; return false; }
