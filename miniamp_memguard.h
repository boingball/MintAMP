#ifndef MINIAMP_MEMGUARD_H
#define MINIAMP_MEMGUARD_H

#include <stddef.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise the one process-wide allocator SignalSemaphore.  Must be called
 * once, on the main task, at true program startup -- before the radio net
 * worker or any playback child task is created -- so every later malloc-family
 * call from any task serialises on the same lock.  No-op off AmigaOS. */
void MiniMem_LockInit(void);

void *MiniMem_Malloc(size_t size, const char *file, int line);
void *MiniMem_Calloc(size_t count, size_t size, const char *file, int line);
void *MiniMem_Realloc(void *ptr, size_t size, const char *file, int line);
void MiniMem_Free(void *ptr, const char *file, int line);
int MiniMem_CheckAll(const char *where);
void MiniMem_ReportLeaks(void);

/* Dump every still-live malloc-family allocation (recursion-safe: printf
 * only).  Call immediately before main() returns so the log lists exactly
 * what libnix ___free_all() will FreeMem() at exit. */
void MiniMem_DumpLive(const char *where);

/* 1 if ptr is a live malloc/calloc/realloc/strdup block from this allocator.
 * Application FreeMem()/FreeVec() sites use it under DEBUG to detect a
 * malloc-family pointer wrongly handed to the exec allocator. */
int MiniMem_Owns(const void *ptr);

#ifdef __cplusplus
}
#endif

#if defined(MINIAMP_DEBUG_ALLOC) && !defined(MINIAMP_MEMGUARD_INTERNAL)
#define malloc(sz) MiniMem_Malloc((sz), __FILE__, __LINE__)
#define calloc(n, sz) MiniMem_Calloc((n), (sz), __FILE__, __LINE__)
#define realloc(ptr, sz) MiniMem_Realloc((ptr), (sz), __FILE__, __LINE__)
#define free(ptr) MiniMem_Free((ptr), __FILE__, __LINE__)
#endif

#endif /* MINIAMP_MEMGUARD_H */
