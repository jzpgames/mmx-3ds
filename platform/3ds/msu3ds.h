#pragma once
/* MSU-1 music for the 3DS port (revision 18).
 *
 * Plays a standard Mega Man X MSU-1 pack (DarkShock numbering: track N =
 * music command - $10, files "<name>-<N>.pcm", 44.1 kHz stereo with the
 * "MSU1" + loop-point header) in place of the SPC music. The SPC keeps the
 * sound effects, so they no longer take channels from the music.
 *
 * Behaviour mirrors DarkShock's patch (github.com/mlarouche/MegamanX-MSU1):
 * a music command whose track exists is consumed (the SPC never hears it),
 * a missing track falls back to the SNES music; $F5 resume becomes an SPC
 * stop + MSU fade-in, $F6 fades out, $FF/$FE duck and restore the volume
 * for the pause menu.
 *
 * The SD card is read by a worker thread into a ring the mixer drains on
 * the main thread (the host build reads synchronously, deterministic). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Scans dir for the pack ("<name>-<N>.pcm", most tracks wins). Returns the
 * number of tracks found; 0 leaves MSU-1 fully inert (stock behaviour). */
unsigned Mmx3dsMsuInit(const char *dir);
/* Registers the game hooks and starts the SD reader (only with a pack). */
void Mmx3dsMsuInstallHooks(void);
void Mmx3dsMsuShutdown(void);
unsigned Mmx3dsMsuTracks(void);
const char *Mmx3dsMsuPackName(void);

/* User switch. Applies from the next song: a song already started on one
 * source keeps playing there. */
void Mmx3dsMsuSetEnabled(bool enabled);
bool Mmx3dsMsuEnabled(void);   /* pack present and switched on */
bool Mmx3dsMsuPlaying(void);   /* an MSU track is the current music */

/* Percent gains: music for the MSU track, effects for the SPC output while
 * the MSU track is the music (the SPC then carries effects only). */
void Mmx3dsMsuSetGains(unsigned music_pct, unsigned sfx_pct);

/* Once per emulated frame, after the game logic: fades. */
void Mmx3dsMsuFrame(void);
/* Adds the music to `frames` stereo frames of SPC output at the native
 * rate (MMX3DS_NATIVE_RATE) and applies the effects gain. */
void Mmx3dsMsuMix(int16_t *pcm, unsigned frames);

/* Savestate trailer. */
enum { MMX3DS_MSU_STATE_SIZE = 32 };
void Mmx3dsMsuSaveState(uint8_t out[MMX3DS_MSU_STATE_SIZE]);
void Mmx3dsMsuLoadState(const uint8_t *in); /* NULL: no trailer -> stop */
