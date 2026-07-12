#define MINIAMP_MEMGUARD_INTERNAL
#include "miniamp_memguard.h"
#include <stdlib.h>

/* ------------------------------------------------------------------------- */
/* Process-wide allocator lock (compiled into BOTH the HEAPGUARD and the      */
/* production build so the wrappers below can serialize the real libnix       */
/* allocator regardless of build).                                            */
/*                                                                            */
/* libnix's malloc/calloc/realloc/free share ONE global allocation list for   */
/* the whole executable, and that list has no native locking -- it is written */
/* for a single-task program.  This player runs malloc-family calls from      */
/* three tasks at once (main/GUI, the radio net worker, and each playback     */
/* child), all against that one list in the shared exe data.  Concurrent      */
/* link/unlink corrupts the list; the damage is invisible to MiniMem's own    */
/* guard bytes (they only cover application buffers) and only surfaces when    */
/* libnix ___free_all() walks the corrupted list after main() returns --      */
/* exactly the recoverable AN_FreeTwice (01000009) / AN_BadFreeAddr           */
/* (0100000F) pair, on the main/CRT task, that survives an otherwise clean     */
/* run.  One SignalSemaphore taken around the real allocator call AND the      */
/* MiniMem list update as a single transaction removes the race.              */
#if defined(__amigaos__) || defined(AMIGA_M68K) || defined(AMIGA) || defined(__AMIGA__)
#include <exec/types.h>
#include <exec/semaphores.h>
#include <proto/exec.h>
static struct SignalSemaphore gMiniMemLock;
static int gMiniMemLockReady;
#define MINIMEM_TASK() ((void *)FindTask(NULL))
/* Before MiniMem_LockInit() the program is still single-task (no worker or
 * child spawned yet), so skipping the semaphore is safe; after it, every
 * task -- including playback children in the same address space -- shares
 * this one lock. */
#define MINIMEM_LOCK()   MiniMem_AllocLock()
#define MINIMEM_UNLOCK() MiniMem_AllocUnlock()
void MiniMem_AllocLock(void) { if (gMiniMemLockReady) ObtainSemaphore(&gMiniMemLock); }
void MiniMem_AllocUnlock(void) { if (gMiniMemLockReady) ReleaseSemaphore(&gMiniMemLock); }
int MiniMem_AllocLockReady(void) { return gMiniMemLockReady; }
void *MiniMem_CurrentTask(void) { return MINIMEM_TASK(); }
void MiniMem_LockInit(void)
{
    InitSemaphore(&gMiniMemLock);
    gMiniMemLockReady = 1;
}
#else
#define MINIMEM_TASK() ((void *)0)
#define MINIMEM_LOCK()   MiniMem_AllocLock()
#define MINIMEM_UNLOCK() MiniMem_AllocUnlock()
void MiniMem_AllocLock(void) { }
void MiniMem_AllocUnlock(void) { }
int MiniMem_AllocLockReady(void) { return 0; }
void *MiniMem_CurrentTask(void) { return MINIMEM_TASK(); }
void MiniMem_LockInit(void) { }
#endif

#if defined(MINIAMP_DEBUG_ALLOC)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MINIMEM_HEAD_MAGIC 0x4d4d4844UL
#define MINIMEM_FREE_MAGIC 0x4d4d4652UL
#define MINIMEM_GUARD_BYTE 0xA5U
#define MINIMEM_FRONT_GUARD 16U
#define MINIMEM_REAR_GUARD 16U

typedef struct MiniMemBlock {
    unsigned long magic;
    size_t size;
    const char *file;
    int line;
    unsigned long seq;      /* monotonic allocation order for the live dump */
    void *task;             /* FindTask(NULL) at allocation time            */
    struct MiniMemBlock *prev;
    struct MiniMemBlock *next;
    unsigned char front[MINIMEM_FRONT_GUARD];
} MiniMemBlock;

static MiniMemBlock *gMiniMemBlocks;
static unsigned long gMiniMemAllocSeq;

static unsigned char *MiniMem_User(MiniMemBlock *b)
{
    return (unsigned char *)(b + 1);
}

static unsigned char *MiniMem_Rear(MiniMemBlock *b)
{
    return MiniMem_User(b) + b->size;
}

static void MiniMem_FillGuard(unsigned char *p, unsigned int n)
{
    unsigned int i;
    for (i = 0; i < n; ++i) p[i] = (unsigned char)MINIMEM_GUARD_BYTE;
}

static int MiniMem_GuardBad(const unsigned char *p, unsigned int n)
{
    unsigned int i;
    for (i = 0; i < n; ++i) {
        if (p[i] != (unsigned char)MINIMEM_GUARD_BYTE) return 1;
    }
    return 0;
}

static int MiniMem_BlockCorrupt(MiniMemBlock *b, const char *where)
{
    int corrupt = 0;
    if (!b || b->magic != MINIMEM_HEAD_MAGIC) return 1;
    if (MiniMem_GuardBad(b->front, MINIMEM_FRONT_GUARD)) {
        printf("MiniMem: front guard corrupt at %p size=%lu alloc=%s:%d where=%s\n",
            (void *)MiniMem_User(b), (unsigned long)b->size, b->file ? b->file : "?", b->line,
            where ? where : "");
        corrupt = 1;
    }
    if (MiniMem_GuardBad(MiniMem_Rear(b), MINIMEM_REAR_GUARD)) {
        printf("MiniMem: rear guard corrupt at %p size=%lu alloc=%s:%d where=%s\n",
            (void *)MiniMem_User(b), (unsigned long)b->size, b->file ? b->file : "?", b->line,
            where ? where : "");
        corrupt = 1;
    }
    return corrupt;
}

/* Bitmask flags for the lock-safe corruption check below. */
#define MINIMEM_BAD_MAGIC 1
#define MINIMEM_BAD_FRONT 2
#define MINIMEM_BAD_REAR  4

/* Corruption check that never printf()s, so it is safe to call while holding
 * gMiniMemLock (the "never printf or allocate while holding the allocator
 * lock" rule).  The caller reports the details after releasing the lock. */
static int MiniMem_BlockCorruptQuiet(MiniMemBlock *b)
{
    int bad = 0;
    if (!b || b->magic != MINIMEM_HEAD_MAGIC) return MINIMEM_BAD_MAGIC;
    if (MiniMem_GuardBad(b->front, MINIMEM_FRONT_GUARD)) bad |= MINIMEM_BAD_FRONT;
    if (MiniMem_GuardBad(MiniMem_Rear(b), MINIMEM_REAR_GUARD)) bad |= MINIMEM_BAD_REAR;
    return bad;
}

static MiniMemBlock *MiniMem_Find(void *ptr)
{
    MiniMemBlock *b;
    for (b = gMiniMemBlocks; b; b = b->next) {
        if (MiniMem_User(b) == (unsigned char *)ptr) return b;
    }
    return NULL;
}

static void MiniMem_Link(MiniMemBlock *b)
{
    b->prev = NULL;
    b->next = gMiniMemBlocks;
    if (gMiniMemBlocks) gMiniMemBlocks->prev = b;
    gMiniMemBlocks = b;
}

static void MiniMem_Unlink(MiniMemBlock *b)
{
    if (b->prev) b->prev->next = b->next;
    else gMiniMemBlocks = b->next;
    if (b->next) b->next->prev = b->prev;
    b->prev = b->next = NULL;
}

/* Allocate + initialise + link, assuming gMiniMemLock is already held: the
 * real libnix malloc() and the MiniMem list insertion happen as one locked
 * transaction so no other task can observe or race a half-linked list. */
static MiniMemBlock *MiniMem_AllocLocked(size_t size, const char *file,
    int line, void *task)
{
    size_t total = sizeof(MiniMemBlock) + size + MINIMEM_REAR_GUARD;
    MiniMemBlock *b = (MiniMemBlock *)malloc(total);
    if (!b) return NULL;
    b->magic = MINIMEM_HEAD_MAGIC;
    b->size = size;
    b->file = file;
    b->line = line;
    b->seq = ++gMiniMemAllocSeq;
    b->task = task;
    MiniMem_FillGuard(b->front, MINIMEM_FRONT_GUARD);
    MiniMem_FillGuard(MiniMem_Rear(b), MINIMEM_REAR_GUARD);
    MiniMem_Link(b);
    return b;
}

/* Unlink + real free, assuming gMiniMemLock is held.  Returns 0 on success,
 * sets *unknown when ptr is not one of ours, otherwise returns the corruption
 * bitmask; the caller reports either condition after unlocking. */
static int MiniMem_FreeLocked(void *ptr, int *unknown)
{
    MiniMemBlock *b = MiniMem_Find(ptr);
    int bad;
    *unknown = 0;
    if (!b) { *unknown = 1; return 0; }
    bad = MiniMem_BlockCorruptQuiet(b);
    if (bad) return bad;
    MiniMem_Unlink(b);
    b->magic = MINIMEM_FREE_MAGIC;
    free(b);                    /* real libnix free, under the same lock */
    return 0;
}

void *MiniMem_Malloc(size_t size, const char *file, int line)
{
    void *task = MINIMEM_TASK();   /* captured outside the lock (no alloc) */
    MiniMemBlock *b;
    MINIMEM_LOCK();
    b = MiniMem_AllocLocked(size, file, line, task);
    MINIMEM_UNLOCK();
    return b ? (void *)MiniMem_User(b) : NULL;
}

void *MiniMem_Calloc(size_t count, size_t size, const char *file, int line)
{
    size_t total = count * size;
    void *p;
    if (count && total / count != size) return NULL;
    p = MiniMem_Malloc(total, file, line);
    if (p) memset(p, 0, total);   /* p is caller-owned now: no lock needed */
    return p;
}

void *MiniMem_Realloc(void *ptr, size_t size, const char *file, int line)
{
    void *task = MINIMEM_TASK();
    MiniMemBlock *old, *nw = NULL;
    void *result = NULL;
    int unknown = 0;
    int bad = 0;
    if (!ptr) return MiniMem_Malloc(size, file, line);
    if (size == 0) {
        MiniMem_Free(ptr, file, line);
        return NULL;
    }
    /* Hold the lock across find-old, alloc-new, copy and free-old so ownership
     * is atomic across the two pointers: another task can never see the old
     * block already gone while the new one is not yet linked, or vice versa. */
    MINIMEM_LOCK();
    old = MiniMem_Find(ptr);
    if (!old) {
        unknown = 1;
    } else {
        bad = MiniMem_BlockCorruptQuiet(old);
        if (!bad) {
            nw = MiniMem_AllocLocked(size, file, line, task);
            if (nw) {
                size_t copy = old->size < size ? old->size : size;
                memcpy(MiniMem_User(nw), MiniMem_User(old), copy);
                MiniMem_Unlink(old);
                old->magic = MINIMEM_FREE_MAGIC;
                free(old);
                result = (void *)MiniMem_User(nw);
            }
        }
    }
    MINIMEM_UNLOCK();
    if (unknown)
        printf("MiniMem: realloc of unknown pointer %p at %s:%d\n", ptr, file ? file : "?", line);
    else if (bad)
        printf("MiniMem: realloc of corrupt block %p at %s:%d flags=%d\n", ptr, file ? file : "?", line, bad);
    return result;
}

void MiniMem_Free(void *ptr, const char *file, int line)
{
    void *freeTask = MINIMEM_TASK();
    int unknown = 0;
    int bad;
    if (!ptr) return;
    MINIMEM_LOCK();
    bad = MiniMem_FreeLocked(ptr, &unknown);
    MINIMEM_UNLOCK();
    if (unknown)
        printf("MiniMem: free of unknown pointer %p at %s:%d freeTask=%p\n",
            ptr, file ? file : "?", line, freeTask);
    else if (bad)
        printf("MiniMem: refusing to free corrupt block %p at %s:%d flags=%d freeTask=%p\n",
            ptr, file ? file : "?", line, bad, freeTask);
}

int MiniMem_CheckAll(const char *where)
{
    MiniMemBlock *b;
    int corrupt = 0;
    for (b = gMiniMemBlocks; b; b = b->next) {
        if (MiniMem_BlockCorrupt(b, where)) ++corrupt;
    }
    if (corrupt > 0) {
        printf("MiniMem: %d corrupt active allocation(s) found where=%s\n", corrupt, where ? where : "");
    }
    return corrupt;
}

void MiniMem_ReportLeaks(void)
{
    MiniMemBlock *b;
    int leaks = 0;
    for (b = gMiniMemBlocks; b; b = b->next) {
        printf("MiniMem: leak seq=%lu %p size=%lu alloc=%s:%d task=%p\n",
            b->seq, (void *)MiniMem_User(b), (unsigned long)b->size,
            b->file ? b->file : "?", b->line, b->task);
        ++leaks;
    }
    if (leaks) printf("MiniMem: %d active allocation(s) leaked\n", leaks);
}

/* Dump every still-live malloc-family allocation this allocator is tracking,
 * in one clearly-delimited block, using only printf() so it never re-enters
 * malloc/free (no recursion into the allocator it is inspecting).  Called
 * immediately before main() returns: whatever it lists is exactly what
 * libnix ___free_all() is about to walk and FreeMem(), so any block here
 * whose task matches the alerting task is a prime AN_FreeTwice candidate. */
void MiniMem_DumpLive(const char *where)
{
    MiniMemBlock *b;
    unsigned long live = 0;
    printf("MiniMem: live-dump BEGIN where=%s\n", where ? where : "");
    for (b = gMiniMemBlocks; b; b = b->next) {
        printf("MiniMem: live seq=%lu op=malloc-family family=libnix ptr=%p size=%lu source=%s:%d task=%p state=live\n",
            b->seq, (void *)MiniMem_User(b), (unsigned long)b->size,
            b->file ? b->file : "?", b->line, b->task);
        ++live;
    }
    printf("MiniMem: live-dump END where=%s live=%lu\n", where ? where : "", live);
}

/* Return 1 if ptr is a live user pointer this allocator handed out, i.e. a
 * malloc/calloc/realloc/strdup block.  Application FreeMem()/FreeVec() sites
 * use this under DEBUG to catch a malloc-family pointer being released with
 * the exec allocator -- the exact cross-family mismatch that leaves a block
 * on libnix's list to be double-freed by ___free_all() at exit. */
int MiniMem_Owns(const void *ptr)
{
    MiniMemBlock *b;
    int found = 0;
    if (!ptr) return 0;
    /* Called during normal operation (from the app's exec-free guard), so a
     * worker/child could be link/unlinking concurrently: take the same lock
     * for a consistent walk.  No printf/alloc here, so locking is safe. */
    MINIMEM_LOCK();
    for (b = gMiniMemBlocks; b; b = b->next) {
        if ((const unsigned char *)MiniMem_User(b) == (const unsigned char *)ptr) { found = 1; break; }
    }
    MINIMEM_UNLOCK();
    return found;
}

#else

/* Production (non-HEAPGUARD) wrappers: no guard bookkeeping, but still take
 * the one process-wide allocator lock around the real libnix call so the
 * shared malloc list stays consistent across the GUI, worker and playback
 * child tasks.  These are only reached when the header's malloc/free
 * redirect is active (i.e. a build that opts into the wrappers). */
void *MiniMem_Malloc(size_t size, const char *file, int line) { void *p; (void)file; (void)line; MINIMEM_LOCK(); p = malloc(size); MINIMEM_UNLOCK(); return p; }
void *MiniMem_Calloc(size_t count, size_t size, const char *file, int line) { void *p; (void)file; (void)line; MINIMEM_LOCK(); p = calloc(count, size); MINIMEM_UNLOCK(); return p; }
void *MiniMem_Realloc(void *ptr, size_t size, const char *file, int line) { void *p; (void)file; (void)line; MINIMEM_LOCK(); p = realloc(ptr, size); MINIMEM_UNLOCK(); return p; }
void MiniMem_Free(void *ptr, const char *file, int line) { (void)file; (void)line; MINIMEM_LOCK(); free(ptr); MINIMEM_UNLOCK(); }
int MiniMem_CheckAll(const char *where) { (void)where; return 0; }
void MiniMem_ReportLeaks(void) { }
void MiniMem_DumpLive(const char *where) { (void)where; }
int MiniMem_Owns(const void *ptr) { (void)ptr; return 0; }

#endif
