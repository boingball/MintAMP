/*
 * wav_alloc.c - libc-free allocation shim for the WAV decoder module.
 *
 * Decoder modules are linked with -nostartfiles and loaded with LoadSeg(),
 * so pulling in the C runtime allocator can introduce startup/exit
 * dependencies that are not available to standalone decoder hunks.  This
 * file backs the small alloc surface wav_module.c needs with Exec
 * AllocMem/FreeMem, without referencing the CRT SysBase/DOSBase globals
 * (see flac_alloc.c / aac_alloc.c / ogg_alloc.c for the same pattern used
 * by this project's other decoder modules).
 */

#include "wav_alloc.h"

#ifndef HAVE_AMIGA_AUDIO_DEVICE
#error "wav_alloc.c is for the Amiga decoder module build and must not fall back to libc allocation"
#endif

#include <exec/types.h>
#include <exec/memory.h>

typedef struct WavAllocHeader {
    unsigned long size;
} WavAllocHeader;

static void *gWavExecBase;

void WavModuleSetExecBase(void *execBase)
{
    gWavExecBase = execBase;
}

static void *WavExecAllocMem(unsigned long bytes, unsigned long flags)
{
    register void *a6 __asm("a6") = gWavExecBase;
    register unsigned long d0 __asm("d0") = bytes;
    register unsigned long d1 __asm("d1") = flags;

    __asm volatile ("jsr a6@(-198:W)"
                    : "+r" (d0)
                    : "r" (d1), "r" (a6)
                    : "a0", "a1", "d2", "cc", "memory");
    return (void *)d0;
}

static void WavExecFreeMem(void *ptr, unsigned long bytes)
{
    register void *a6 __asm("a6") = gWavExecBase;
    register void *a1 __asm("a1") = ptr;
    register unsigned long d0 __asm("d0") = bytes;

    __asm volatile ("jsr a6@(-210:W)"
                    :
                    : "r" (a1), "r" (d0), "r" (a6)
                    : "a0", "d1", "d2", "cc", "memory");
}

void *WavModuleCalloc(size_t count, size_t bytes)
{
    unsigned long payload = (unsigned long)count * (unsigned long)bytes;
    unsigned long total;
    WavAllocHeader *hdr;

    if (bytes != 0 && payload / (unsigned long)bytes != (unsigned long)count)
        return NULL;

    total = payload + (unsigned long)sizeof(WavAllocHeader);
    if (total < payload || !gWavExecBase)
        return NULL;

    hdr = (WavAllocHeader *)WavExecAllocMem(total, MEMF_FAST | MEMF_CLEAR);
    if (!hdr)
        return NULL;

    hdr->size = payload;
    return (void *)(hdr + 1);
}

void WavModuleFree(void *ptr)
{
    WavAllocHeader *hdr;
    unsigned long total;

    if (!ptr)
        return;

    hdr = ((WavAllocHeader *)ptr) - 1;
    total = hdr->size + (unsigned long)sizeof(WavAllocHeader);
    WavExecFreeMem(hdr, total);
}
