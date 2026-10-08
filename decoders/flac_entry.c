/*
 * flac_entry.c - FLAC decoder module entry point.
 *
 * This file MUST be compiled and linked FIRST so that DecoderModuleEntry
 * lands at BADDR(seg)+4 — the first callable LONG in the code hunk.
 * Compile order: flac_entry.c flac_module.c flac/<foxen_flac.c>
 */

#include "decoder_module.h"


#ifdef HAVE_AMIGA_AUDIO_DEVICE
/* Exec base is kept in flac_alloc.c under a private symbol so the decoder
 * does not export or link the usual CRT SysBase/DOSBase globals. */
extern void FlacModuleSetExecBase(void *execBase);
#endif

/* Forward declaration — defined in flac_module.c */
extern struct DecoderOps gFlacOps;

/*
 * DecoderModuleEntry — module entry point, called by the host after LoadSeg().
 * Initialises SysBase from the well-known Amiga address 4 and returns the
 * static vtable.
 */
__attribute__((section(".text")))
struct DecoderOps *DecoderModuleEntry(void)
{
#ifdef HAVE_AMIGA_AUDIO_DEVICE
    FlacModuleSetExecBase(DecoderModuleAbsExecBase());
#endif
    return &gFlacOps;
}
