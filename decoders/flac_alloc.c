/*
 * flac_alloc.c - libc-free allocation shim for FLAC decoder modules.
 *
 * FLAC modules are linked with -nostartfiles and loaded with LoadSeg(), so
 * pulling the C runtime allocator can introduce startup/exit dependencies that
 * are not available to standalone decoder hunks.  This file provides the small
 * malloc/free/calloc/realloc/exit surface needed by third-party decoder code and
 * backs it with Exec AllocMem/FreeMem for Amiga builds.
 */

#define FLAC_ALLOC_IMPLEMENTATION
#include "flac_alloc.h"

#ifdef HAVE_AMIGA_AUDIO_DEVICE
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>

extern struct ExecBase *SysBase;

typedef struct FlacAllocHeader {
    unsigned long size;
} FlacAllocHeader;

static void FlacModuleZero(void *ptr, unsigned long bytes)
{
    unsigned char *p = (unsigned char *)ptr;
    while (bytes--)
        *p++ = 0;
}

static void FlacModuleCopy(void *dst, const void *src, unsigned long bytes)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (bytes--)
        *d++ = *s++;
}

void *FlacModuleMalloc(size_t bytes)
{
    unsigned long payload = (unsigned long)bytes;
    unsigned long total = payload + (unsigned long)sizeof(FlacAllocHeader);
    FlacAllocHeader *hdr;

    if (total < payload)
        return NULL;

    hdr = (FlacAllocHeader *)AllocMem(total, MEMF_FAST | MEMF_CLEAR);
    if (!hdr)
        return NULL;

    hdr->size = payload;
    return (void *)(hdr + 1);
}

void *FlacModuleCalloc(size_t count, size_t bytes)
{
    size_t total = count * bytes;
    if (bytes != 0 && total / bytes != count)
        return NULL;
    return FlacModuleMalloc(total);
}

void *FlacModuleRealloc(void *ptr, size_t bytes)
{
    FlacAllocHeader *oldHdr;
    void *newPtr;
    unsigned long oldSize;
    unsigned long copySize;

    if (!ptr)
        return FlacModuleMalloc(bytes);
    if (bytes == 0) {
        FlacModuleFree(ptr);
        return NULL;
    }

    oldHdr = ((FlacAllocHeader *)ptr) - 1;
    oldSize = oldHdr->size;
    newPtr = FlacModuleMalloc(bytes);
    if (!newPtr)
        return NULL;

    copySize = oldSize < (unsigned long)bytes ? oldSize : (unsigned long)bytes;
    FlacModuleCopy(newPtr, ptr, copySize);
    if ((unsigned long)bytes > copySize)
        FlacModuleZero((unsigned char *)newPtr + copySize, (unsigned long)bytes - copySize);
    FlacModuleFree(ptr);
    return newPtr;
}

void FlacModuleFree(void *ptr)
{
    FlacAllocHeader *hdr;
    unsigned long total;

    if (!ptr)
        return;

    hdr = ((FlacAllocHeader *)ptr) - 1;
    total = hdr->size + (unsigned long)sizeof(FlacAllocHeader);
    FreeMem(hdr, total);
}

void FlacModuleExit(int status)
{
    (void)status;
    for (;;)
        ;
}

/*
 * Provide libc-compatible symbol names for third-party FLAC code that calls
 * malloc/free directly.  Because these definitions are part of the decoder
 * module, the linker does not need to pull malloc.o or exit/_exit support from
 * the C runtime.
 */
void *malloc(size_t bytes)
{
    return FlacModuleMalloc(bytes);
}

void *calloc(size_t count, size_t bytes)
{
    return FlacModuleCalloc(count, bytes);
}

void *realloc(void *ptr, size_t bytes)
{
    return FlacModuleRealloc(ptr, bytes);
}

void free(void *ptr)
{
    FlacModuleFree(ptr);
}

void exit(int status)
{
    FlacModuleExit(status);
}

#else
#include <stdlib.h>
#include <string.h>

void *FlacModuleMalloc(size_t bytes)
{
    return malloc(bytes);
}

void *FlacModuleCalloc(size_t count, size_t bytes)
{
    return calloc(count, bytes);
}

void *FlacModuleRealloc(void *ptr, size_t bytes)
{
    return realloc(ptr, bytes);
}

void FlacModuleFree(void *ptr)
{
    free(ptr);
}

void FlacModuleExit(int status)
{
    exit(status);
}
#endif
