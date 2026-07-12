#define MINIAMP_MEMGUARD_INTERNAL
#include "libnix_alloc_audit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(MINIAMP_DEBUG_ALLOC) && defined(MINIAMP_LINKER_ALLOC_AUDIT)

#ifndef LIBNIX_ALLOC_AUDIT_MAX
#define LIBNIX_ALLOC_AUDIT_MAX 16384U
#endif

typedef enum LibnixAuditState {
    LIBNIX_AUDIT_EMPTY = 0,
    LIBNIX_AUDIT_LIVE,
    LIBNIX_AUDIT_FREED
} LibnixAuditState;

typedef struct LibnixAuditEntry {
    unsigned long seq;
    const char *op;
    void *ptr;
    size_t size;
    void *alloc_task;
    void *free_task;
    void *caller;
    LibnixAuditState state;
} LibnixAuditEntry;

static LibnixAuditEntry gAudit[LIBNIX_ALLOC_AUDIT_MAX];
static unsigned long gSeq;
static unsigned long gLive;
static unsigned long gUnknownFree;
static unsigned long gDoubleFree;
static unsigned long gUnknownRealloc;

extern void *__real_malloc(size_t size);
extern void *__real_calloc(size_t count, size_t size);
extern void *__real_realloc(void *ptr, size_t size);
extern void __real_free(void *ptr);
void *__wrap_malloc(size_t size);
void *__wrap_calloc(size_t count, size_t size);
void *__wrap_realloc(void *oldptr, size_t size);
void __wrap_free(void *ptr);

#if defined(__GNUC__)
#define AUDIT_CALLER() __builtin_return_address(0)
#else
#define AUDIT_CALLER() ((void *)0)
#endif

static const char *AuditStateName(LibnixAuditState state)
{
    switch (state) {
    case LIBNIX_AUDIT_LIVE: return "live";
    case LIBNIX_AUDIT_FREED: return "freed";
    default: return "empty";
    }
}

static long AuditFindPtr(void *ptr)
{
    unsigned int i;
    for (i = 0; i < LIBNIX_ALLOC_AUDIT_MAX; ++i) {
        if (gAudit[i].ptr == ptr && gAudit[i].state != LIBNIX_AUDIT_EMPTY) return (long)i;
    }
    return -1;
}

static long AuditFindSlot(void)
{
    unsigned int i;
    for (i = 0; i < LIBNIX_ALLOC_AUDIT_MAX; ++i) {
        if (gAudit[i].state == LIBNIX_AUDIT_EMPTY || gAudit[i].state == LIBNIX_AUDIT_FREED) return (long)i;
    }
    return -1;
}

static void AuditRecordAlloc(const char *op, void *ptr, size_t size, void *task, void *caller)
{
    long slot;
    if (!ptr) return;
    slot = AuditFindSlot();
    if (slot < 0) return;
    gAudit[slot].seq = ++gSeq;
    gAudit[slot].op = op;
    gAudit[slot].ptr = ptr;
    gAudit[slot].size = size;
    gAudit[slot].alloc_task = task;
    gAudit[slot].free_task = NULL;
    gAudit[slot].caller = caller;
    gAudit[slot].state = LIBNIX_AUDIT_LIVE;
    ++gLive;
}

void *__wrap_malloc(size_t size)
{
    void *task = MiniMem_CurrentTask();
    void *caller = AUDIT_CALLER();
    void *ptr;
    MiniMem_AllocLock();
    ptr = __real_malloc(size);
    AuditRecordAlloc("LIBNIX-ALLOC", ptr, size, task, caller);
    MiniMem_AllocUnlock();
    return ptr;
}

void *__wrap_calloc(size_t count, size_t size)
{
    void *task = MiniMem_CurrentTask();
    void *caller = AUDIT_CALLER();
    size_t total = count * size;
    void *ptr;
    if (count && total / count != size) total = 0;
    MiniMem_AllocLock();
    ptr = __real_calloc(count, size);
    AuditRecordAlloc("LIBNIX-CALLOC", ptr, total, task, caller);
    MiniMem_AllocUnlock();
    return ptr;
}

void *__wrap_realloc(void *oldptr, size_t size)
{
    void *task = MiniMem_CurrentTask();
    void *caller = AUDIT_CALLER();
    long oldslot = -1;
    void *newptr;
    int unknown = 0;
    if (!oldptr) return __wrap_malloc(size);
    if (size == 0) { __wrap_free(oldptr); return NULL; }
    MiniMem_AllocLock();
    oldslot = AuditFindPtr(oldptr);
    if (oldslot < 0 || gAudit[oldslot].state != LIBNIX_AUDIT_LIVE) unknown = 1;
    newptr = __real_realloc(oldptr, size);
    if (newptr) {
        if (!unknown) {
            gAudit[oldslot].state = LIBNIX_AUDIT_FREED;
            gAudit[oldslot].free_task = task;
            --gLive;
        } else {
            ++gUnknownRealloc;
        }
        AuditRecordAlloc("LIBNIX-REALLOC", newptr, size, task, caller);
    }
    MiniMem_AllocUnlock();
    if (unknown) printf("LIBNIX-REALLOC-UNKNOWN seq=%lu oldptr=%p ptr=%p size=%lu task=%p caller=%p state=unknown\n", gSeq, oldptr, newptr, (unsigned long)size, task, caller);
    return newptr;
}

void __wrap_free(void *ptr)
{
    void *task = MiniMem_CurrentTask();
    void *caller = AUDIT_CALLER();
    long slot;
    int unknown = 0, dbl = 0;
    if (!ptr) return;
    MiniMem_AllocLock();
    slot = AuditFindPtr(ptr);
    if (slot < 0) { unknown = 1; ++gUnknownFree; }
    else if (gAudit[slot].state == LIBNIX_AUDIT_FREED) { dbl = 1; ++gDoubleFree; }
    else {
        gAudit[slot].state = LIBNIX_AUDIT_FREED;
        gAudit[slot].free_task = task;
        --gLive;
    }
    __real_free(ptr);
    MiniMem_AllocUnlock();
    if (unknown) printf("LIBNIX-FREE-UNKNOWN seq=%lu ptr=%p task=%p caller=%p state=unknown\n", gSeq, ptr, task, caller);
    else if (dbl) printf("LIBNIX-DOUBLE-FREE seq=%lu ptr=%p task=%p caller=%p state=freed\n", gSeq, ptr, task, caller);
}

unsigned long LibnixAllocAudit_LiveCount(void) { return gLive; }

void LibnixAllocAudit_DumpLive(const char *where)
{
    unsigned int i;
    printf("LIBNIX-LIVE-SUMMARY where=%s live=%lu unknown_free=%lu double_free=%lu unknown_realloc=%lu\n",
        where ? where : "", gLive, gUnknownFree, gDoubleFree, gUnknownRealloc);
    for (i = 0; i < LIBNIX_ALLOC_AUDIT_MAX; ++i) {
        if (gAudit[i].state == LIBNIX_AUDIT_LIVE) {
            printf("LIBNIX-LIVE seq=%lu ptr=%p size=%lu alloc_task=%p free_task=%p caller=%p state=%s op=%s\n",
                gAudit[i].seq, gAudit[i].ptr, (unsigned long)gAudit[i].size,
                gAudit[i].alloc_task, gAudit[i].free_task, gAudit[i].caller,
                AuditStateName(gAudit[i].state), gAudit[i].op ? gAudit[i].op : "?");
        }
    }
}

#else
unsigned long LibnixAllocAudit_LiveCount(void) { return 0; }
void LibnixAllocAudit_DumpLive(const char *where) { (void)where; }
#endif
