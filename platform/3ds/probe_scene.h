#pragma once
#include <stdbool.h>

/* Original geometric artwork; no game ROM, game CPU, audio or game assets. */
void MmxProbeInit(void);
bool MmxProbeCapture(int actor_x, unsigned scroll);
