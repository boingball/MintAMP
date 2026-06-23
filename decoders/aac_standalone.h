#ifndef DECODERS_AAC_STANDALONE_H
#define DECODERS_AAC_STANDALONE_H

/*
 * Forced-include shim for the standalone Amiga AAC decoder build.
 *
 * The decoder module is linked with -nostartfiles and must not pull newlib's
 * assert/abort/raise/dummy objects.  Define NDEBUG before assert.h is parsed
 * and keep assert as a local no-op for the libhelix-aac source.
 *
 * SBR (Spectral Band Replication / HE-AAC) is disabled by default via the
 * ESP8266 guard in aaccommon.h — this matches the "no SBR" profile used on
 * memory-constrained platforms and saves ~30 KB Fast RAM.  Remove the
 * ESP8266 define below and add the sbr*.c files to AAC_LIB_SRCS to enable it.
 */
#ifndef NDEBUG
#define NDEBUG 1
#endif

/* Disable SBR: aaccommon.h sets AAC_ENABLE_SBR only when ESP8266 is absent. */
#ifndef ESP8266
#define ESP8266
#endif

#include "aac_alloc.h"
#include <assert.h>

#ifdef assert
#undef assert
#endif
#define assert(x) ((void)0)

#endif /* DECODERS_AAC_STANDALONE_H */
