#ifndef DECODERS_IFF_ALLOC_H
#define DECODERS_IFF_ALLOC_H

#include <stddef.h>

/*
 * iff_alloc.h - tiny Exec-backed allocator for the IFF/8SVX decoder module.
 *
 * Same rationale as wav_alloc.h: this module has no vendored third-party
 * source calling the libc malloc/calloc/realloc/free names, so there is no
 * need to shadow those symbols; iff_module.c calls these by their own name.
 */

void *IffModuleCalloc(size_t count, size_t bytes);
void  IffModuleFree(void *ptr);
void  IffModuleSetExecBase(void *execBase);

#endif /* DECODERS_IFF_ALLOC_H */
