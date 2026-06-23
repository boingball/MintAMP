#ifndef DECODERS_AAC_ALLOC_H
#define DECODERS_AAC_ALLOC_H

#include <stddef.h>

void *AacModuleMalloc(size_t bytes);
void *AacModuleCalloc(size_t count, size_t bytes);
void *AacModuleRealloc(void *ptr, size_t bytes);
void  AacModuleFree(void *ptr);
#if defined(__GNUC__)
void  AacModuleExit(int status) __attribute__((noreturn));
#else
void  AacModuleExit(int status);
#endif
void  AacModuleSetExecBase(void *execBase);

#endif /* DECODERS_AAC_ALLOC_H */
