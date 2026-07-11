/*
 * iff_entry.c - IFF/8SVX decoder module entry point.
 *
 * This file MUST be compiled and linked FIRST so that DecoderModuleEntry
 * lands at BADDR(seg)+4 — the first callable LONG in the code hunk.
 * Compile order: iff_entry.c iff_alloc.c iff_module.c
 */

#include "decoder_module.h"

#ifdef HAVE_AMIGA_AUDIO_DEVICE
extern void IffModuleSetExecBase(void *execBase);
#endif

/* Forward declaration — defined in iff_module.c */
extern struct DecoderOps gIffOps;

/*
 * DecoderModuleEntry — module entry point, called by the host after LoadSeg().
 * Initialises SysBase from the well-known Amiga address 4 and returns the
 * static vtable.
 */
__attribute__((section(".text")))
struct DecoderOps *DecoderModuleEntry(void)
{
#ifdef HAVE_AMIGA_AUDIO_DEVICE
    IffModuleSetExecBase(*((void **)4L));
#endif
    return &gIffOps;
}
