/*
 * flac_entry.c - FLAC decoder module entry point.
 *
 * This file MUST be compiled and linked FIRST so that DecoderModuleEntry
 * lands at BADDR(seg)+4 — the first callable LONG in the code hunk.
 * Compile order: flac_entry.c flac_module.c flac/<foxen_flac.c>
 */

#include "decoder_module.h"

#include <stdio.h>

#ifdef HAVE_AMIGA_AUDIO_DEVICE
#include <exec/types.h>
#include <exec/execbase.h>

/* SysBase must be set before any exec library calls (AllocMem etc.)     */
/* We define it here so every file in this module sees the same instance. */
struct ExecBase *SysBase;
#endif

/* Forward declaration — defined in flac_module.c */
extern struct DecoderOps gFlacOps;

/*
 * DecoderModuleEntry — module entry point, called by the host after LoadSeg().
 * Initialises SysBase from the well-known Amiga address 4 and returns the
 * static vtable.
 */
struct DecoderOps *DecoderModuleEntry(void)
{
#ifdef HAVE_AMIGA_AUDIO_DEVICE
    SysBase = *((struct ExecBase **)4L);
#endif
    printf("FLAC MODULE BUILD MARKER 12345\n");
    fflush(stdout);
    return &gFlacOps;
}
