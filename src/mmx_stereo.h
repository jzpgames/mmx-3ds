#pragma once
#include <stdint.h>

/* Presentation only: positive binocular disparity puts the selected scene
 * plane behind the screen. Normally this is BG2; Sigma 3 uses shallow BG1.
 * OBJ (actors/HUD) and BG3 (text/effects) stay at zero disparity. A setting
 * describes ONE eye of ONE immutable frame. Integer samples preserve art. */
typedef enum MmxStereoEye {
  MMX_STEREO_LEFT = -1, MMX_STEREO_MONO = 0, MMX_STEREO_RIGHT = 1
} MmxStereoEye;
enum { MMX_STEREO_FULL = 1024, MMX_STEREO_MAX_OFFSET = 6,
       MMX_STEREO_MAX_SOURCE_OFFSET = 10 };

/* Source-pixel separation at the top of the hardware slider. The highway
 * keeps its original depth; enclosed and underwater backgrounds use a
 * stronger separation; Sigma 3 instead uses a shallow two-pixel scene plane.
 * This changes presentation only, never stage scroll, collisions, actors or
 * the HUD. Unknown scenes keep the original profile. */
static inline unsigned MmxStereoStageMaxOffset(unsigned stage) {
  static const uint8_t limits[] = {6, 10, 8, 10, 9, 8, 9, 8, 8, 8, 9, 2, 8};
  return stage < sizeof(limits) ? limits[stage] : MMX_STEREO_MAX_OFFSET;
}
/* Sigma 3's entrance is painted over BG2 on BG1: shifting BG2 has no visible
 * effect. Use a shallow scene plane there, keeping actors and HUD at zero.
 * Elsewhere BG1 remains fixed and the distant BG2 is separated. */
static inline unsigned MmxStereoStageLayer(unsigned stage) {
  return stage == 11 ? 0 : 1;
}

static inline int MmxStereoStageOffset(MmxStereoEye eye, unsigned amount,
                                       unsigned stage) {
  if (eye != MMX_STEREO_LEFT && eye != MMX_STEREO_RIGHT) return 0;
  if (amount > MMX_STEREO_FULL) amount = MMX_STEREO_FULL;
  int pixels = (int)((amount * MmxStereoStageMaxOffset(stage) +
                      MMX_STEREO_FULL / 2) / MMX_STEREO_FULL);
  return eye == MMX_STEREO_LEFT ? pixels : -pixels;
}

static inline int MmxStereoBackgroundOffset(MmxStereoEye eye, unsigned amount) {
  if (eye != MMX_STEREO_LEFT && eye != MMX_STEREO_RIGHT) return 0;
  if (amount > MMX_STEREO_FULL) amount = MMX_STEREO_FULL;
  int pixels = (int)((amount * MMX_STEREO_MAX_OFFSET + MMX_STEREO_FULL / 2) /
                     MMX_STEREO_FULL);
  /* Sampling to the right moves an image to the left. */
  return eye == MMX_STEREO_LEFT ? pixels : -pixels;
}
