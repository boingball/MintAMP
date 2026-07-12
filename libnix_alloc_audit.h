#ifndef LIBNIX_ALLOC_AUDIT_H
#define LIBNIX_ALLOC_AUDIT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void LibnixAllocAudit_DumpLive(const char *where);
unsigned long LibnixAllocAudit_LiveCount(void);
void MiniMem_AllocLock(void);
void MiniMem_AllocUnlock(void);
int MiniMem_AllocLockReady(void);
void *MiniMem_CurrentTask(void);

#ifdef __cplusplus
}
#endif

#endif /* LIBNIX_ALLOC_AUDIT_H */
