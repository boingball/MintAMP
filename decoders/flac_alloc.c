/*
 * flac_alloc.c - libc-free allocation shim for FLAC decoder modules.
 *
 * FLAC modules are linked with -nostartfiles and loaded with LoadSeg(), so
 * pulling the C runtime allocator can introduce startup/exit dependencies that
 * are not available to standalone decoder hunks.  This file provides the small
 * malloc/free/calloc/realloc surface needed by third-party decoder code and
 * backs it with Exec AllocMem/FreeMem for Amiga builds without referencing the
 * CRT SysBase/DOSBase globals.
 */

#define FLAC_ALLOC_IMPLEMENTATION
#include "flac_alloc.h"

#ifndef HAVE_AMIGA_AUDIO_DEVICE
#error "flac_alloc.c is for the Amiga decoder module build and must not fall back to libc allocation"
#endif

#include <exec/types.h>
#include <exec/memory.h>

typedef struct FlacAllocHeader {
    unsigned long size;
} FlacAllocHeader;

static void *gFlacExecBase;

void FlacModuleSetExecBase(void *execBase)
{
    gFlacExecBase = execBase;
}

static void *FlacExecAllocMem(unsigned long bytes, unsigned long flags)
{
    register void *a6 __asm("a6") = gFlacExecBase;
    register unsigned long d0 __asm("d0") = bytes;
    register unsigned long d1 __asm("d1") = flags;

    __asm volatile ("jsr a6@(-198:W)"
                    : "+r" (d0)
                    : "r" (d1), "r" (a6)
                    : "a0", "a1", "d2", "cc", "memory");
    return (void *)d0;
}

static void FlacExecFreeMem(void *ptr, unsigned long bytes)
{
    register void *a6 __asm("a6") = gFlacExecBase;
    register void *a1 __asm("a1") = ptr;
    register unsigned long d0 __asm("d0") = bytes;

    __asm volatile ("jsr a6@(-210:W)"
                    :
                    : "r" (a1), "r" (d0), "r" (a6)
                    : "a0", "d1", "d2", "cc", "memory");
}

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

    if (total < payload || !gFlacExecBase)
        return NULL;

    hdr = (FlacAllocHeader *)FlacExecAllocMem(total, MEMF_FAST | MEMF_CLEAR);
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
    FlacExecFreeMem(hdr, total);
}

void FlacModuleExit(int status)
{
    (void)status;
    for (;;)
        ;
}

/*
 * Provide libc-compatible allocation symbol names for third-party FLAC code
 * that calls them directly.  These definitions live in the decoder module so
 * the linker does not need libc allocator objects.
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

/*
 * Some newlib-built objects call the reentrant allocator entry points instead
 * of the plain ANSI names.  Keep those references inside this module too; do
 * not let the linker satisfy them from libc.a(malloc.o).
 */
void *_malloc_r(void *reent, size_t bytes)
{
    (void)reent;
    return FlacModuleMalloc(bytes);
}

void *_calloc_r(void *reent, size_t count, size_t bytes)
{
    (void)reent;
    return FlacModuleCalloc(count, bytes);
}

void *_realloc_r(void *reent, void *ptr, size_t bytes)
{
    (void)reent;
    return FlacModuleRealloc(ptr, bytes);
}

void _free_r(void *reent, void *ptr)
{
    (void)reent;
    FlacModuleFree(ptr);
}

void exit(int status)
{
    FlacModuleExit(status);
}
