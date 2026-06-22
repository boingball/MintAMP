#ifndef DECODERS_FLAC_ALLOC_H
#define DECODERS_FLAC_ALLOC_H

#include <stddef.h>

void *FlacModuleMalloc(size_t bytes);
void *FlacModuleCalloc(size_t count, size_t bytes);
void *FlacModuleRealloc(void *ptr, size_t bytes);
void  FlacModuleFree(void *ptr);
void  FlacModuleExit(int status);


#endif /* DECODERS_FLAC_ALLOC_H */
