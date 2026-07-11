/*
 * iff_alloc.c - libc-free allocation shim for the IFF/8SVX decoder module.
 *
 * See wav_alloc.c / flac_alloc.c for the shared rationale: decoder modules
 * link with -nostartfiles and are loaded with LoadSeg(), so allocation is
 * backed directly by Exec AllocMem/FreeMem without touching CRT
 * SysBase/DOSBase globals.
 */

#include "iff_alloc.h"

#ifndef HAVE_AMIGA_AUDIO_DEVICE
#error "iff_alloc.c is for the Amiga decoder module build and must not fall back to libc allocation"
#endif

#include <exec/types.h>
#include <exec/memory.h>

typedef struct IffAllocHeader {
    unsigned long size;
} IffAllocHeader;

static void *gIffExecBase;

void IffModuleSetExecBase(void *execBase)
{
    gIffExecBase = execBase;
}

static void *IffExecAllocMem(unsigned long bytes, unsigned long flags)
{
    register void *a6 __asm("a6") = gIffExecBase;
    register unsigned long d0 __asm("d0") = bytes;
    register unsigned long d1 __asm("d1") = flags;

    __asm volatile ("jsr a6@(-198:W)"
                    : "+r" (d0)
                    : "r" (d1), "r" (a6)
                    : "a0", "a1", "d2", "cc", "memory");
    return (void *)d0;
}

static void IffExecFreeMem(void *ptr, unsigned long bytes)
{
    register void *a6 __asm("a6") = gIffExecBase;
    register void *a1 __asm("a1") = ptr;
    register unsigned long d0 __asm("d0") = bytes;

    __asm volatile ("jsr a6@(-210:W)"
                    :
                    : "r" (a1), "r" (d0), "r" (a6)
                    : "a0", "d1", "d2", "cc", "memory");
}

void *IffModuleCalloc(size_t count, size_t bytes)
{
    unsigned long payload = (unsigned long)count * (unsigned long)bytes;
    unsigned long total;
    IffAllocHeader *hdr;

    if (bytes != 0 && payload / (unsigned long)bytes != (unsigned long)count)
        return NULL;

    total = payload + (unsigned long)sizeof(IffAllocHeader);
    if (total < payload || !gIffExecBase)
        return NULL;

    hdr = (IffAllocHeader *)IffExecAllocMem(total, MEMF_FAST | MEMF_CLEAR);
    if (!hdr)
        return NULL;

    hdr->size = payload;
    return (void *)(hdr + 1);
}

void IffModuleFree(void *ptr)
{
    IffAllocHeader *hdr;
    unsigned long total;

    if (!ptr)
        return;

    hdr = ((IffAllocHeader *)ptr) - 1;
    total = hdr->size + (unsigned long)sizeof(IffAllocHeader);
    IffExecFreeMem(hdr, total);
}
