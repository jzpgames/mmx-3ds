/* 1.0.2: mmx-log.txt is written by its own thread.
 *
 * stderr used to go straight to the SD card from the main thread, one write
 * per line. An SD card sometimes takes a second or more to accept a write;
 * the game then waited, and since the next performance line was due again
 * by the time it resumed, it could stall for several seconds (1.0.2 test log:
 * four freezes of 3-4 s with nothing slow in the game or the 3D).
 *
 * Now stderr points at a small device ("mmxlog:") whose write only copies
 * into a ring in memory. A low-priority thread on the same core moves the
 * text to the card whenever the main thread waits. If the card stalls, the
 * ring absorbs up to RING bytes; beyond that lines are dropped and counted,
 * never waited for. */
#include "logq.h"

#include <3ds.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/iosupport.h>

enum { RING = 256 * 1024, CHUNK = 16 * 1024 };

static char ring[RING];
static size_t head, tail; /* head: next write, tail: next read; head - tail = used */
static unsigned long dropped;
static LightLock lock;
static LightEvent wake;
static Thread writer;
static volatile bool quitting;
static FILE *target;

static int dev_open(struct _reent *r, void *file, const char *path, int flags, int mode) {
  (void)r; (void)file; (void)path; (void)flags; (void)mode;
  return 0;
}
static int dev_close(struct _reent *r, void *fd) { (void)r; (void)fd; return 0; }
static ssize_t dev_write(struct _reent *r, void *fd, const char *ptr, size_t len) {
  (void)r; (void)fd;
  LightLock_Lock(&lock);
  size_t room = RING - (head - tail), n = len < room ? len : room;
  for (size_t i = 0; i < n;) {
    size_t at = head % RING, run = RING - at;
    if (run > n - i) run = n - i;
    memcpy(ring + at, ptr + i, run);
    head += run; i += run;
  }
  if (n < len) ++dropped;
  LightLock_Unlock(&lock);
  LightEvent_Signal(&wake);
  return (ssize_t)len; /* never block the caller */
}

static const devoptab_t device = {
  .name = "mmxlog", .structSize = sizeof(int),
  .open_r = dev_open, .close_r = dev_close, .write_r = dev_write,
};

/* Moves everything queued to the card. Returns false when nothing was queued. */
static bool drain(void) {
  static char out[CHUNK];
  bool any = false;
  for (;;) {
    LightLock_Lock(&lock);
    size_t used = head - tail, at = tail % RING, n = used < CHUNK ? used : CHUNK;
    if (n > RING - at) n = RING - at;
    memcpy(out, ring + at, n);
    tail += n;
    unsigned long lost = dropped; dropped = 0;
    LightLock_Unlock(&lock);
    if (lost) fprintf(target, "LOG dropped_writes=%lu (SD card busy)\n", lost);
    if (!n) break;
    fwrite(out, 1, n, target);
    any = true;
  }
  if (any) fflush(target);
  return any;
}

static void writer_main(void *unused) {
  (void)unused;
  while (!quitting) {
    LightEvent_WaitTimeout(&wake, 500000000LL);
    drain();
  }
  drain();
}

bool LogQueueStart(const char *path) {
  target = fopen(path, "w");
  if (!target) return false;
  setvbuf(target, NULL, _IOFBF, CHUNK);
  LightLock_Init(&lock);
  LightEvent_Init(&wake, RESET_ONESHOT);
  if (AddDevice(&device) < 0) { fclose(target); target = NULL; return false; }
  s32 priority = 0x30;
  svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
  /* Lower priority than the game (higher number), same core: it only runs
   * while the main thread waits for the composer or a VBlank. */
  writer = threadCreate(writer_main, NULL, 16 * 1024, priority < 0x3f ? priority + 1 : priority, 0, false);
  if (!writer || !freopen("mmxlog:", "w", stderr)) {
    if (writer) { quitting = true; LightEvent_Signal(&wake); threadJoin(writer, U64_MAX); threadFree(writer); writer = NULL; }
    fclose(target); target = NULL;
    return false;
  }
  setvbuf(stderr, NULL, _IOLBF, 4096);
  return true;
}

void LogQueueStop(void) {
  if (!writer) return;
  fflush(stderr);
  quitting = true;
  LightEvent_Signal(&wake);
  threadJoin(writer, U64_MAX);
  threadFree(writer);
  writer = NULL;
  fclose(target);
  target = NULL;
}
