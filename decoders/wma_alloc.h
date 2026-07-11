#ifndef DECODERS_WMA_ALLOC_H
#define DECODERS_WMA_ALLOC_H

#include <stddef.h>

/*
 * wma_alloc.h - Exec-backed allocator plus the freestanding mem-family and
 * qsort primitives the vendored decoders/wma/ sources need (see decoders/wma/
 * codeclib.h for why: those sources call memcpy/memset/memmove/memcmp/
 * qsort by their plain libc names, and decoders/Makefile's wma.decoder
 * rule redirects those names here with -Dmemcpy=WmaMemcpy etc., the same
 * forced-include pattern flac_alloc.h/aac_alloc.h/ogg_alloc.h use for
 * malloc/calloc/free).
 */

void *WmaModuleCalloc(size_t count, size_t bytes);
void  WmaModuleFree(void *ptr);
void  WmaModuleSetExecBase(void *execBase);

void *WmaMemcpy(void *dest, const void *src, size_t n);
void *WmaMemset(void *s, int c, size_t n);
void *WmaMemmove(void *dest, const void *src, size_t n);
int   WmaMemcmp(const void *s1, const void *s2, size_t n);
void  WmaQsort(void *base, size_t nmemb, size_t size,
               int (*compar)(const void *, const void *));

#endif /* DECODERS_WMA_ALLOC_H */
