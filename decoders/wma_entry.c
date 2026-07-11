/*
 * wma_entry.c - WMA decoder module entry point.
 *
 * This file MUST be compiled and linked FIRST so that DecoderModuleEntry
 * lands at BADDR(seg)+4 — the first callable LONG in the code hunk.
 * Compile order: wma_entry.c wma_alloc.c wma_asf.c, the decoders/wma/
 * sources, then wma_module.c
 */

#include "decoder_module.h"

#ifdef HAVE_AMIGA_AUDIO_DEVICE
extern void WmaModuleSetExecBase(void *execBase);
#endif

/* Forward declaration — defined in wma_module.c */
extern struct DecoderOps gWmaOps;

/*
 * DecoderModuleEntry — module entry point, called by the host after LoadSeg().
 * Initialises SysBase from the well-known Amiga address 4 and returns the
 * static vtable.
 */
__attribute__((section(".text")))
struct DecoderOps *DecoderModuleEntry(void)
{
#ifdef HAVE_AMIGA_AUDIO_DEVICE
    WmaModuleSetExecBase(*((void **)4L));
#endif
    return &gWmaOps;
}
