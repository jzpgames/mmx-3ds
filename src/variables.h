/* Game-specific RAM variable declarations for Mega Man X.
 *
 * Scaffold: just the framework-protocol variables the runner expects.
 * Populate with named MMX RAM regions as they're identified (DP-slot
 * vars, sprite-table bases, etc.) — for now the recompiled gen code
 * refers to memory by raw address.
 */

#ifndef VARIABLES_H
#define VARIABLES_H

#include "types.h"

/* g_ram is declared by snesrecomp/runner/src/common_rtl.h. */

/* Host-protocol frame counters, populated by the orchestration in
 * mmx_rtl.c. Framework-shaped, not game-specific. */
extern uint16 counter_global_frames;

/* MMX's NMI vblank flag is at $7E:0B9D — verified from the main-loop
 * spinlock at $00:80A1 (`LDA $0B9D ; BEQ -3`) and the NMI handler at
 * $00:8195 (`LDA #$FF ; STA $0B9D`). NMI sets it to $FF; the main
 * loop reads it, then (eventually) zeros it during dispatch. Setting
 * this to non-zero before calling MainLoop short-circuits the
 * spinlock so the C host can drive the loop one frame at a time. */
#define waiting_for_vblank (*(uint8*)(g_ram + 0x0B9D))

#endif /* VARIABLES_H */
