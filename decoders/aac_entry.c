/*
 * aac_entry.c - AAC decoder module entry point.
 *
 * This file MUST be compiled and linked FIRST so that DecoderModuleEntry
 * lands at BADDR(seg)+4 — the first callable LONG in the code hunk.
 * Compile order: aac_entry.c aac_alloc.c aac_module.c aac/<library>.c ...
 */

#include "decoder_module.h"

extern struct DecoderOps gAacOps;
/*
 * Keep the LoadSeg entry path as small as possible.  The host crash report
 * shows failures before the returned ops table can be validated, so avoid
 * touching ExecBase or running any AAC setup here.  AacOpen() initialises the
 * module allocator before the first allocation.
 */
__attribute__((section(".text")))
struct DecoderOps *DecoderModuleEntry(void)
{
    return &gAacOps;
}
