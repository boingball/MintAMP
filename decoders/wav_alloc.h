#ifndef DECODERS_WAV_ALLOC_H
#define DECODERS_WAV_ALLOC_H

#include <stddef.h>

/*
 * wav_alloc.h - tiny Exec-backed allocator for the WAV decoder module.
 *
 * Unlike flac_alloc.c/aac_alloc.c/ogg_alloc.c, this module contains no
 * vendored third-party source that calls the libc malloc/calloc/realloc/free
 * names directly, so there is no need to shadow those symbols or provide the
 * newlib _r reentrant variants.  wav_module.c calls these functions by their
 * own names instead.
 */

void *WavModuleCalloc(size_t count, size_t bytes);
void  WavModuleFree(void *ptr);
void  WavModuleSetExecBase(void *execBase);

#endif /* DECODERS_WAV_ALLOC_H */
