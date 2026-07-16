/* amiga_tasksafe_malloc.c -- process-wide task-safe C allocator (AmigaOS/m68k)
 * ===========================================================================
 *
 * Why this file exists
 * --------------------
 * This program runs several tasks inside ONE loaded image that all share a
 * single C runtime:
 *
 *     * the GUI main task (Intuition/GadTools event loop, redraws, list and
 *       string bookkeeping -- amiga_mp3gui.c / minimp3r.c),
 *     * every playback child (CreateNewProcTags(NP_Entry=PlaybackEntry); the
 *       decoder amiga_mp3dec.c performs dozens of malloc/free per run), and
 *     * the long-lived AmiSSL/bsdsocket net worker.
 *
 * libnix's malloc/free/calloc/realloc use one global, UNLOCKED arena. It is
 * not task-safe. When any two of those tasks are inside malloc/free at the
 * same time -- e.g. the decoder freeing a buffer on the playback child while
 * the GUI allocates a redraw structure, both of which happen constantly while
 * a station is playing -- the arena's free-list bookkeeping races. The loser
 * eventually hands a malformed block to Exec's FreeMem, which corrupts the
 * real Exec free list. That damage is frequently latent: the bad bytes land
 * in memory that is still allocated (or not yet re-linked into a free chunk),
 * so a heap walk stays clean until later AllocMem/FreeMem churn (for instance
 * the buffer traffic inside a subsequent SSL_connect) finally links the
 * malformed chunk onto the free list -- which is exactly where the first
 * "bad chunk size" is reported, even though nothing was racing at that
 * instant. This is why cleanup-order, private-vs-shared SSL_CTX and teardown
 * experiments never moved the fault, and why the standalone repro -- a
 * separate executable with its own C runtime touched by exactly one task --
 * never reproduces it.
 *
 * The fix
 * -------
 * Replace the C allocator wholesale with strong malloc/calloc/realloc/free
 * symbols backed directly by Exec AllocVec/FreeVec. AllocVec/FreeVec serialise
 * internally in Exec (they take the memory lists' own arbitration), so every
 * task in the image can allocate and free concurrently with no shared-arena
 * race -- the same isolation the standalone worker gets for free, now given to
 * the GUI task, every playback child and the net worker alike, in ALL builds
 * (not only HEAPGUARD). Because these strong symbols win at link time, they
 * also capture libnix's own internal callers of the public malloc symbol
 * (e.g. stdio buffer allocation, strdup), closing that path too. And because
 * libnix's private arena is now never populated, its ___free_all exit walk
 * finds an empty list, which also removes the close-time AN_BadFreeAddr the
 * crt_freeall_probe was chasing.
 *
 * A small fixed prefix records each block's usable size (so realloc can copy
 * the right amount) while keeping the returned pointer 8-byte aligned.
 *
 * Toggle: defined(AMIGA_TASKSAFE_MALLOC), set by Makefile.amiga for every
 * linked Amiga target. Build with TASKSAFEMALLOC=0 to fall back to libnix's
 * allocator (e.g. to A/B test, or if a future toolchain rejects the override).
 */

/* Non-empty translation unit even when the override is compiled out. */
typedef int amiga_tasksafe_malloc_unit;

#if defined(AMIGA_M68K) && defined(AMIGA_TASKSAFE_MALLOC)

#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <stdlib.h>
#include <string.h>

/* 8-byte prefix: [0..3] usable size, [4..7] pad. AllocVec returns 8-aligned
 * memory, so returning base+8 keeps user pointers 8-aligned. FreeVec is given
 * back exactly base = userptr - TSM_PREFIX. */
#define TSM_PREFIX 8U

void *malloc(size_t size)
{
    unsigned char *base;
    if (size == 0) size = 1;
    base = (unsigned char *)AllocVec((ULONG)size + TSM_PREFIX, MEMF_ANY);
    if (!base) return NULL;
    *(unsigned long *)base = (unsigned long)size;
    return base + TSM_PREFIX;
}

void free(void *ptr)
{
    if (!ptr) return;
    FreeVec((unsigned char *)ptr - TSM_PREFIX);
}

void *calloc(size_t nmemb, size_t size)
{
    size_t total;
    void *p;
    if (nmemb != 0 && size != 0) {
        total = nmemb * size;
        if (total / nmemb != size) return NULL; /* overflow */
    } else {
        total = 0;
    }
    p = malloc(total);
    if (p && total) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t size)
{
    void *np;
    size_t old;
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }
    old = (size_t)*(unsigned long *)((unsigned char *)ptr - TSM_PREFIX);
    if (old >= size) return ptr;              /* shrink/keep: reuse in place */
    np = malloc(size);
    if (!np) return NULL;
    memcpy(np, ptr, old);
    free(ptr);
    return np;
}

#endif /* AMIGA_M68K && AMIGA_TASKSAFE_MALLOC */
