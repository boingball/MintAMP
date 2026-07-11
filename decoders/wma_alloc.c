/*
 * wma_alloc.c - Exec-backed allocator + freestanding mem/qsort primitives
 * for the WMA decoder module.  See wma_alloc.h and decoders/wma/codeclib.h
 * for why these exist (no libc is linked into -nostartfiles decoder
 * modules; same rationale as flac_alloc.c/aac_alloc.c/ogg_alloc.c).
 */

#include "wma_alloc.h"

#ifndef HAVE_AMIGA_AUDIO_DEVICE
#error "wma_alloc.c is for the Amiga decoder module build and must not fall back to libc allocation"
#endif

#include <exec/types.h>
#include <exec/memory.h>

typedef struct WmaAllocHeader {
    unsigned long size;
} WmaAllocHeader;

static void *gWmaExecBase;

void WmaModuleSetExecBase(void *execBase)
{
    gWmaExecBase = execBase;
}

static void *WmaExecAllocMem(unsigned long bytes, unsigned long flags)
{
    register void *a6 __asm("a6") = gWmaExecBase;
    register unsigned long d0 __asm("d0") = bytes;
    register unsigned long d1 __asm("d1") = flags;

    __asm volatile ("jsr a6@(-198:W)"
                    : "+r" (d0)
                    : "r" (d1), "r" (a6)
                    : "a0", "a1", "d2", "cc", "memory");
    return (void *)d0;
}

static void WmaExecFreeMem(void *ptr, unsigned long bytes)
{
    register void *a6 __asm("a6") = gWmaExecBase;
    register void *a1 __asm("a1") = ptr;
    register unsigned long d0 __asm("d0") = bytes;

    __asm volatile ("jsr a6@(-210:W)"
                    :
                    : "r" (a1), "r" (d0), "r" (a6)
                    : "a0", "d1", "d2", "cc", "memory");
}

void *WmaModuleCalloc(size_t count, size_t bytes)
{
    unsigned long payload = (unsigned long)count * (unsigned long)bytes;
    unsigned long total;
    WmaAllocHeader *hdr;

    if (bytes != 0 && payload / (unsigned long)bytes != (unsigned long)count)
        return NULL;

    total = payload + (unsigned long)sizeof(WmaAllocHeader);
    if (total < payload || !gWmaExecBase)
        return NULL;

    hdr = (WmaAllocHeader *)WmaExecAllocMem(total, MEMF_FAST | MEMF_CLEAR);
    if (!hdr)
        return NULL;

    hdr->size = payload;
    return (void *)(hdr + 1);
}

void WmaModuleFree(void *ptr)
{
    WmaAllocHeader *hdr;
    unsigned long total;

    if (!ptr)
        return;

    hdr = ((WmaAllocHeader *)ptr) - 1;
    total = hdr->size + (unsigned long)sizeof(WmaAllocHeader);
    WmaExecFreeMem(hdr, total);
}

void *WmaMemcpy(void *dest, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    while (n--)
        *d++ = *s++;
    return dest;
}

void *WmaMemset(void *s, int c, size_t n)
{
    unsigned char *p = (unsigned char *)s;
    unsigned char v = (unsigned char)c;
    while (n--)
        *p++ = v;
    return s;
}

void *WmaMemmove(void *dest, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;

    if (d == s || n == 0)
        return dest;

    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dest;
}

int WmaMemcmp(const void *s1, const void *s2, size_t n)
{
    const unsigned char *a = (const unsigned char *)s1;
    const unsigned char *b = (const unsigned char *)s2;
    while (n--) {
        if (*a != *b)
            return (int)*a - (int)*b;
        a++;
        b++;
    }
    return 0;
}

/*
 * Non-recursive diagnostic sort used while investigating the WMA startup
 * #80000006 failure on real m68k hardware.  The previous first-element-pivot
 * quicksort could recurse once per item for already ordered VLC data and
 * exhaust the playback task stack.  WMA only sorts these tables at decoder
 * initialisation, so the slower insertion sort is acceptable for this test.
 */
static void WmaSwapBytes(unsigned char *a, unsigned char *b, size_t size)
{
    size_t i;
    for (i = 0; i < size; i++) {
        unsigned char t = a[i];
        a[i] = b[i];
        b[i] = t;
    }
}

static void WmaInsertionSort(unsigned char *base, size_t nmemb, size_t size,
                              int (*compar)(const void *, const void *))
{
    size_t i, j;
    for (i = 1; i < nmemb; i++) {
        j = i;
        while (j > 0 && compar(base + (j - 1) * size, base + j * size) > 0) {
            WmaSwapBytes(base + (j - 1) * size, base + j * size, size);
            j--;
        }
    }
}

void WmaQsort(void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *))
{
    if (!base || nmemb < 2 || size == 0)
        return;
    WmaInsertionSort((unsigned char *)base, nmemb, size, compar);
}
