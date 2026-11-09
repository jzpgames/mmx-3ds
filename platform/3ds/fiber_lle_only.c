/* This target selects the upstream fiber-free LLE scheduler. Fail loudly if
 * a legacy HLE path is entered; never fake a successful fiber switch. */
#include "fiber_compat.h"
#include <stdio.h>
#include <stdlib.h>
static void unsupported(void) {
  fputs("El planificador HLE no esta disponible en 3DS. Use LLE.\n", stderr); abort();
}
void *ConvertThreadToFiber(void *p) { (void)p; unsupported(); return NULL; }
void *CreateFiber(size_t n, FiberProc f, void *p) { (void)n; (void)f; (void)p; unsupported(); return NULL; }
void SwitchToFiber(void *f) { (void)f; unsupported(); }
void DeleteFiber(void *f) { if (f) unsupported(); }
unsigned long GetLastError(void) { return 0; }
int FiberSnapshotSupported(void) { return 0; }
size_t FiberSnapshotBound(void *f) { (void)f; return 0; }
size_t FiberSnapshotSave(void *f, void *out, size_t n) { (void)f; (void)out; (void)n; return 0; }
int FiberSnapshotLoad(void *f, const void *in, size_t n) { (void)f; (void)in; (void)n; return 0; }
