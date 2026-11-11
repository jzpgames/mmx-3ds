#ifndef MMX3DS_LOGQ_H
#define MMX3DS_LOGQ_H
#include <stdbool.h>
/* 1.0.2: stderr to `path` through a writer thread (see logq.c). */
bool LogQueueStart(const char *path);
/* Writes what is still queued and closes the file. */
void LogQueueStop(void);
#endif
