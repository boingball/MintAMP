#define MINIAMP_MEMGUARD_INTERNAL
#include "miniamp_memguard.h"
#include <stdlib.h>

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

void *MiniMem_Malloc(size_t size, const char *file, int line)
{
    size_t total = sizeof(MiniMemBlock) + size + MINIMEM_REAR_GUARD;
    MiniMemBlock *b = (MiniMemBlock *)malloc(total);
    (void)gMiniMemAllocSeq;
    if (!b) return NULL;
    b->magic = MINIMEM_HEAD_MAGIC;
    b->size = size;
    b->file = file;
    b->line = line;
    MiniMem_FillGuard(b->front, MINIMEM_FRONT_GUARD);
    MiniMem_FillGuard(MiniMem_Rear(b), MINIMEM_REAR_GUARD);
    MiniMem_Link(b);
    ++gMiniMemAllocSeq;
    return MiniMem_User(b);
}

void *MiniMem_Calloc(size_t count, size_t size, const char *file, int line)
{
    size_t total = count * size;
    void *p;
    if (count && total / count != size) return NULL;
    p = MiniMem_Malloc(total, file, line);
    if (p) memset(p, 0, total);
    return p;
}

void *MiniMem_Realloc(void *ptr, size_t size, const char *file, int line)
{
    MiniMemBlock *old;
    void *p;
    size_t copy;
    if (!ptr) return MiniMem_Malloc(size, file, line);
    if (size == 0) {
        MiniMem_Free(ptr, file, line);
        return NULL;
    }
    old = MiniMem_Find(ptr);
    if (!old) {
        printf("MiniMem: realloc of unknown pointer %p at %s:%d\n", ptr, file ? file : "?", line);
        return NULL;
    }
    if (MiniMem_BlockCorrupt(old, "realloc")) return NULL;
    p = MiniMem_Malloc(size, file, line);
    if (!p) return NULL;
    copy = old->size < size ? old->size : size;
    memcpy(p, ptr, copy);
    MiniMem_Free(ptr, file, line);
    return p;
}

void MiniMem_Free(void *ptr, const char *file, int line)
{
    MiniMemBlock *b;
    if (!ptr) return;
    b = MiniMem_Find(ptr);
    if (!b) {
        printf("MiniMem: free of unknown pointer %p at %s:%d\n", ptr, file ? file : "?", line);
        return;
    }
    if (MiniMem_BlockCorrupt(b, "free")) {
        printf("MiniMem: refusing to free corrupt block %p at %s:%d\n", ptr, file ? file : "?", line);
        return;
    }
    MiniMem_Unlink(b);
    b->magic = MINIMEM_FREE_MAGIC;
    free(b);
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
        printf("MiniMem: leak %p size=%lu alloc=%s:%d\n",
            (void *)MiniMem_User(b), (unsigned long)b->size, b->file ? b->file : "?", b->line);
        ++leaks;
    }
    if (leaks) printf("MiniMem: %d active allocation(s) leaked\n", leaks);
}

#else

void *MiniMem_Malloc(size_t size, const char *file, int line) { (void)file; (void)line; return malloc(size); }
void *MiniMem_Calloc(size_t count, size_t size, const char *file, int line) { (void)file; (void)line; return calloc(count, size); }
void *MiniMem_Realloc(void *ptr, size_t size, const char *file, int line) { (void)file; (void)line; return realloc(ptr, size); }
void MiniMem_Free(void *ptr, const char *file, int line) { (void)file; (void)line; free(ptr); }
int MiniMem_CheckAll(const char *where) { (void)where; return 0; }
void MiniMem_ReportLeaks(void) { }

#endif
