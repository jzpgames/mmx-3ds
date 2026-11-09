#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mmx_renderer.h"
#include "probe_scene.h"

/* The upstream compositor uses sizeable scanline scratch arrays. */
u32 __stacksize__ = 512 * 1024;

static void present(const uint32_t *rgb, gfx3dSide_t side) {
  u8 *fb = gfxGetFramebuffer(GFX_TOP, side, NULL, NULL);
  /* 400x240 panel; preserve 256x224 integer pixels, centered at (72,8).
   * libctru framebuffers are rotated, column-major, BGR8. */
  memset(fb, 0, 400 * 240 * 3);
  for (unsigned y = 0; y < 224; ++y) for (unsigned x = 0; x < 256; ++x) {
    uint32_t c = rgb[y * 256 + x];
    unsigned offset = ((x + 72) * 240 + 239 - (y + 8)) * 3;
    fb[offset] = (u8)c; fb[offset + 1] = (u8)(c >> 8); fb[offset + 2] = (u8)(c >> 16);
  }
}

int main(void) {
  gfxInitDefault(); gfxSet3D(true);
  consoleInit(GFX_BOTTOM, NULL);
  osSetSpeedupEnable(true);
  uint32_t *image = malloc(256 * 224 * sizeof(*image));
  if (!image) {
    printf("No se pudo reservar memoria.\nSTART: salir\n");
    while (aptMainLoop()) {
      hidScanInput(); if (hidKeysDown() & KEY_START) break;
      gspWaitForVBlank();
    }
    gfxExit(); return 1;
  }
  MmxProbeInit();
  const MmxRenderView view = {256, 0, 256.0 / 224};
  int actor = 112; unsigned tick = 0;
  bool stereo = true, paused = false, ok = true;
  printf("MMX 3DS: prueba del compositor\n\n");
  printf("ESCENA SINTETICA, NO ES EL JUEGO\n\n");
  printf("Deslizador: profundidad de BG2\n");
  printf("BG1, objetos y HUD: plano pantalla\n\n");
  printf("Izq/der: mover marcador\n");
  printf("A: detener/reanudar fondo\n");
  printf("SELECT: activar/desactivar 3D\n");
  printf("START: salir\n\n");
  printf("Sin ROM, sin CPU del juego, sin audio.\n");
  printf("Compilacion experimental.\n");
  while (aptMainLoop()) {
    hidScanInput(); u32 down = hidKeysDown(), held = hidKeysHeld();
    if (down & KEY_START) break;
    if (down & KEY_SELECT) stereo = !stereo;
    if (down & KEY_A) paused = !paused;
    if (held & KEY_LEFT) --actor;
    if (held & KEY_RIGHT) ++actor;
    if (actor < 0) actor = 0;
    if (actor > 240) actor = 240;
    if (!paused) ++tick;
    float slider = osGet3DSliderState();
    if (!(slider > 0)) slider = 0;
    if (slider > 1) slider = 1;
    unsigned amount = stereo ? (unsigned)(slider * MMX_STEREO_FULL + 0.5f) : 0;
    u64 start = svcGetSystemTick();
    ok = MmxProbeCapture(actor, tick / 4);
    if (ok) ok = MmxRendererDrawEye(image, view, true, MMX_STEREO_LEFT, amount);
    if (!ok) break;
    present(image, GFX_LEFT);
    /* With no disparity, reuse the exact mono image. No duplicate update. */
    if (amount) ok = MmxRendererDrawEye(image, view, true, MMX_STEREO_RIGHT, amount);
    if (!ok) break;
    present(image, GFX_RIGHT);
    double ms = (svcGetSystemTick() - start) * 1000.0 / SYSCLOCK_ARM11;
    if ((tick & 15) == 0 || paused)
      printf("\x1b[21;1H3D: %3u%%   render: %6.1f ms  ", amount * 100 / MMX_STEREO_FULL, ms);
    gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
  }
  free(image); gfxExit(); return ok ? 0 : 1;
}
