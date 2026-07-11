/*
 * codeclib.h - minimal stand-in for Rockbox's lib/rbcodec/codecs/lib/codeclib.h.
 *
 * The real header pulls in the whole Rockbox codec-plugin framework
 * (replaygain, codec_malloc, profiling hooks) via codecs.h/ci->..., none of
 * which the vendored decode path (wmadeci.c, wmafixed.c, mdct.c,
 * mdct_lookup.c, fft-ffmpeg.c) actually calls -- confirmed by grepping for
 * ci-> across every vendored file; the only hits were in the real
 * codeclib.c, which this project does not vendor/compile.
 *
 * What's genuinely needed:
 *   - memcpy/memset/memmove/memcmp/qsort: redirected at compile time via
 *     -Dmemcpy=WmaMemcpy etc. (see decoders/Makefile), implemented in
 *     wma_alloc.c. This header only needs to type-check the call sites;
 *     the -D substitution rewrites the identifiers before the compiler
 *     ever resolves them, so the prototypes below use the plain libc
 *     names and get silently renamed along with every call site.
 *   - av_log2: small enough to just define here rather than route through
 *     the alloc shim.
 *   - ff_imdct_calc/ff_imdct_half/ff_fft_calc_c: declared by mdct.h/fft.h
 *     (included directly by the files that need them) and defined in the
 *     vendored mdct.c/fft-ffmpeg.c -- no separate declaration needed here.
 *
 * Not part of upstream Rockbox.
 */
#ifndef MINIAMP3_WMA_CODECLIB_H
#define MINIAMP3_WMA_CODECLIB_H

#include <stddef.h>
#include "platform.h"

void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
void *memmove(void *dest, const void *src, size_t n);
int   memcmp(const void *s1, const void *s2, size_t n);
void  qsort(void *base, size_t nmemb, size_t size,
            int (*compar)(const void *, const void *));

static inline int av_log2(unsigned int v)
{
    int n = 0;
    if (v == 0)
        return 0;
    while (v >>= 1)
        n++;
    return n;
}

#endif /* MINIAMP3_WMA_CODECLIB_H */
