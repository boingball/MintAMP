/*
 * wav_entry.c - WAV decoder module entry point.
 *
 * This file MUST be compiled and linked FIRST so that DecoderModuleEntry
 * lands at BADDR(seg)+4 — the first callable LONG in the code hunk.
 * Compile order: wav_entry.c wav_alloc.c wav_module.c
 */

#include "decoder_module.h"

#ifdef HAVE_AMIGA_AUDIO_DEVICE
extern void WavModuleSetExecBase(void *execBase);
#endif

/* Forward declaration — defined in wav_module.c */
extern struct DecoderOps gWavOps;

/*
 * DecoderModuleEntry — module entry point, called by the host after LoadSeg().
 * Initialises SysBase from the well-known Amiga address 4 and returns the
 * static vtable.
 */
__attribute__((section(".text")))
struct DecoderOps *DecoderModuleEntry(void)
{
#ifdef HAVE_AMIGA_AUDIO_DEVICE
    WavModuleSetExecBase(*((void **)4L));
#endif
    return &gWavOps;
}
