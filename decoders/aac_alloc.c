/*
 * aac_alloc.c - libc-free allocation shim for the AAC decoder module.
 *
 * AAC decoder modules are linked with -nostartfiles and loaded with LoadSeg(),
 * so pulling the C runtime allocator introduces startup/exit dependencies that
 * are not available to standalone decoder hunks.  This file provides the small
 * malloc/free/calloc/realloc surface needed by libhelix-aac and backs it with
 * Exec AllocMem/FreeMem without referencing the CRT SysBase/DOSBase globals.
 */

#define AAC_ALLOC_IMPLEMENTATION
#include "aac_alloc.h"

#ifndef HAVE_AMIGA_AUDIO_DEVICE
#error "aac_alloc.c is for the Amiga decoder module build and must not fall back to libc allocation"
#endif

#include <exec/types.h>
#include <exec/memory.h>

typedef struct AacAllocHeader {
    unsigned long size;
} AacAllocHeader;

static void *gAacExecBase;

void AacModuleSetExecBase(void *execBase)
{
    gAacExecBase = execBase;
}

static void *AacExecAllocMem(unsigned long bytes, unsigned long flags)
{
    register void *a6 __asm("a6") = gAacExecBase;
    register unsigned long d0 __asm("d0") = bytes;
    register unsigned long d1 __asm("d1") = flags;

    __asm volatile ("jsr a6@(-198:W)"
                    : "+r" (d0)
                    : "r" (d1), "r" (a6)
                    : "a0", "a1", "d2", "cc", "memory");
    return (void *)d0;
}

static void AacExecFreeMem(void *ptr, unsigned long bytes)
{
    register void *a6 __asm("a6") = gAacExecBase;
    register void *a1 __asm("a1") = ptr;
    register unsigned long d0 __asm("d0") = bytes;

    __asm volatile ("jsr a6@(-210:W)"
                    :
                    : "r" (a1), "r" (d0), "r" (a6)
                    : "a0", "d1", "d2", "cc", "memory");
}

static void AacModuleZero(void *ptr, unsigned long bytes)
{
    volatile unsigned char *p = (volatile unsigned char *)ptr;
    while (bytes--)
        *p++ = 0;
}

static void AacModuleCopy(void *dst, const void *src, unsigned long bytes)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (bytes--)
        *d++ = *s++;
}

void *AacModuleMalloc(size_t bytes)
{
    unsigned long payload = (unsigned long)bytes;
    unsigned long total = payload + (unsigned long)sizeof(AacAllocHeader);
    AacAllocHeader *hdr;

    if (total < payload || !gAacExecBase)
        return NULL;

    hdr = (AacAllocHeader *)AacExecAllocMem(total, MEMF_FAST | MEMF_CLEAR);
    if (!hdr)
        return NULL;

    hdr->size = payload;
    return (void *)(hdr + 1);
}

void *AacModuleCalloc(size_t count, size_t bytes)
{
    size_t total = count * bytes;
    if (bytes != 0 && total / bytes != count)
        return NULL;
    return AacModuleMalloc(total);
}

void *AacModuleRealloc(void *ptr, size_t bytes)
{
    AacAllocHeader *oldHdr;
    void *newPtr;
    unsigned long oldSize;
    unsigned long copySize;

    if (!ptr)
        return AacModuleMalloc(bytes);
    if (bytes == 0) {
        AacModuleFree(ptr);
        return NULL;
    }

    oldHdr = ((AacAllocHeader *)ptr) - 1;
    oldSize = oldHdr->size;
    newPtr = AacModuleMalloc(bytes);
    if (!newPtr)
        return NULL;

    copySize = oldSize < (unsigned long)bytes ? oldSize : (unsigned long)bytes;
    AacModuleCopy(newPtr, ptr, copySize);
    if ((unsigned long)bytes > copySize)
        AacModuleZero((unsigned char *)newPtr + copySize, (unsigned long)bytes - copySize);
    AacModuleFree(ptr);
    return newPtr;
}

void AacModuleFree(void *ptr)
{
    AacAllocHeader *hdr;
    unsigned long total;

    if (!ptr)
        return;

    hdr = ((AacAllocHeader *)ptr) - 1;
    total = hdr->size + (unsigned long)sizeof(AacAllocHeader);
    AacExecFreeMem(hdr, total);
}

void AacModuleExit(int status)
{
    (void)status;
    for (;;)
        ;
}

/*
 * Provide libc-compatible symbol names for libhelix-aac code that calls them
 * directly.  Redirect them all to our Exec-backed shim.
 */
void *malloc(size_t bytes)
{
    return AacModuleMalloc(bytes);
}

void *calloc(size_t count, size_t bytes)
{
    return AacModuleCalloc(count, bytes);
}

void *realloc(void *ptr, size_t bytes)
{
    return AacModuleRealloc(ptr, bytes);
}

void free(void *ptr)
{
    AacModuleFree(ptr);
}

void *_malloc_r(void *reent, size_t bytes)
{
    (void)reent;
    return AacModuleMalloc(bytes);
}

void *_calloc_r(void *reent, size_t count, size_t bytes)
{
    (void)reent;
    return AacModuleCalloc(count, bytes);
}

void *_realloc_r(void *reent, void *ptr, size_t bytes)
{
    (void)reent;
    return AacModuleRealloc(ptr, bytes);
}

void _free_r(void *reent, void *ptr)
{
    (void)reent;
    AacModuleFree(ptr);
}

#if defined(__GNUC__)
void exit(int status) __attribute__((noreturn));
#endif
void exit(int status)
{
    AacModuleExit(status);
    for (;;)
        ;
}
