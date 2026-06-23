/*
 * aac_entry.c - AAC decoder module entry point.
 *
 * This file MUST be compiled and linked FIRST so that DecoderModuleEntry
 * lands at BADDR(seg)+4 — the first callable LONG in the code hunk.
 * Compile order: aac_entry.c aac_alloc.c aac_module.c aac/<library>.c ...
 */

#include "decoder_module.h"

#ifdef HAVE_AMIGA_AUDIO_DEVICE
extern void AacModuleSetExecBase(void *execBase);
#endif

extern struct DecoderOps gAacOps;

struct DecoderOps *DecoderModuleEntry(void)
{
#ifdef HAVE_AMIGA_AUDIO_DEVICE
    AacModuleSetExecBase(*((void **)4L));
#endif
    return &gAacOps;
}
