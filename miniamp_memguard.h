#ifndef MINIAMP_MEMGUARD_H
#define MINIAMP_MEMGUARD_H

#include <stddef.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

void *MiniMem_Malloc(size_t size, const char *file, int line);
void *MiniMem_Calloc(size_t count, size_t size, const char *file, int line);
void *MiniMem_Realloc(void *ptr, size_t size, const char *file, int line);
void MiniMem_Free(void *ptr, const char *file, int line);
int MiniMem_CheckAll(const char *where);
void MiniMem_ReportLeaks(void);

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
