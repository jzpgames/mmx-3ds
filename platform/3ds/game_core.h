#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "mmx_stereo.h"

enum { MMX3DS_WIDTH = 256, MMX3DS_HEIGHT = 224, MMX3DS_RATE = 32040 };
bool Mmx3dsLoad(const char *rom_path, char *error, size_t capacity);
/* One guest frame = logic, then the PPU frame (HDMA/IRQ, optional capture).
 * Mmx3dsStep(input) is RunLogic + RunRaster(true). */
bool Mmx3dsStep(uint16_t input);
bool Mmx3dsRunLogic(uint16_t input);
bool Mmx3dsRunRaster(bool present);
/* The captured frame will use the native stereo pair path, which only reads
 * the snapshot and may therefore run on another core. */
bool Mmx3dsPairIsNative(void);
bool Mmx3dsDraw(uint32_t *pixels, MmxStereoEye eye, unsigned amount);
bool Mmx3dsDrawPair(uint32_t *left, uint32_t *right, unsigned amount);
typedef struct Mmx3dsFrameTiming {
  double logic_ms, raster_ms, stereo_ms;
} Mmx3dsFrameTiming;
Mmx3dsFrameTiming Mmx3dsTiming(void);
/* Legacy output is retained solely for differential validation on HOST. */
void Mmx3dsAudio(int16_t *pcm, unsigned samples);
bool Mmx3dsNativeAudioEnabled(void);
unsigned Mmx3dsAudioAvailable(void);
unsigned Mmx3dsAudioRead(int16_t*,unsigned);
/* Functional APU/SPC/DSP only; excludes the host PCM ring/cursors. */
uint64_t Mmx3dsAudioStateHash(void);
#define MMX3DS_NATIVE_RATE (534.0 * 60.0988)
unsigned Mmx3dsFrame(void);
double Mmx3dsFramePeriods(void);
void Mmx3dsShutdown(void);
void Mmx3dsLogState(FILE *stream);
/* Savestates. A file holds a small header (stage, time, thumbnail) and the
 * runtime snapshot. Only call between guest frames with nothing reading the
 * capture (after the composer finished). */
enum { MMX3DS_THUMB_W = 88, MMX3DS_THUMB_H = 20 };
typedef struct Mmx3dsStateInfo {
  bool used;
  uint8_t stage;
  int64_t time; /* Seconds since 1970, local time as the 3DS keeps it. */
  uint16_t thumb[MMX3DS_THUMB_W * MMX3DS_THUMB_H]; /* RGB565 */
} Mmx3dsStateInfo;
/* RGB565 thumbnail from a 256x224 XRGB image (a centre band). */
void Mmx3dsMakeThumb(const uint32_t *image, uint16_t *thumb);
bool Mmx3dsSaveState(const char *path, const uint16_t *thumb, char *error, size_t capacity);
bool Mmx3dsLoadState(const char *path, char *error, size_t capacity);
bool Mmx3dsReadStateInfo(const char *path, Mmx3dsStateInfo *info);
uint8_t Mmx3dsStage(void);
