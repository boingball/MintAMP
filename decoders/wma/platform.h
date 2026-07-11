/*
 * platform.h - minimal stand-in for Rockbox's firmware/export/config*.h +
 * various platform headers, providing just what the vendored libwma/
 * lib/rbcodec sources (decoders/wma/ *.c files, from Rockbox's
 * lib/rbcodec/codecs/{libwma,lib}) need to compile freestanding for the
 * MiniAMP3 decoder-module build.  Not part of upstream Rockbox.
 */
#ifndef MINIAMP3_WMA_PLATFORM_H
#define MINIAMP3_WMA_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

/* m68k is big-endian; the host-side test harness (AMIGA_M68K undefined)
 * builds little-endian to exercise the other branch of every
 * ROCKBOX_BIG_ENDIAN conditional in the vendored sources. */
#if defined(AMIGA_M68K)
#define ROCKBOX_BIG_ENDIAN 1
#else
#define ROCKBOX_LITTLE_ENDIAN 1
#endif

/* IRAM/ICODE/const-section placement hints -- meaningless here, no-op.
 * Only the base names: wmadec.h defines its own IBSS_ATTR_WMA_LARGE_IRAM /
 * IBSS_ATTR_WMA_XL_IRAM / ICONST_ATTR_WMA_XL_IRAM (empty, in its final
 * #else branch, since CONFIG_CPU never matches a known target here) --
 * defining those a second time here is harmless but redundant and trips
 * -Wall's macro-redefinition warning. */
#define IBSS_ATTR
#define ICONST_ATTR
#define ICODE_ATTR
#define ICODE_ATTR_TREMOR_MDCT
#define IDATA_ATTR
#define MEM_ALIGN_ATTR
#define NO_PROF_ATTR

/* Branch-prediction hints used by mdct.c / fft-ffmpeg.c. */
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

/* Rockbox debug logging -- discarded (decoder modules link -nostartfiles,
 * no stdio). */
#define DEBUGF(...) ((void)0)

/* Portable byte-swap primitives used by bswap_16/32 (see ffmpeg_get_bits.h /
 * ffmpeg_bswap.h) -- explicit byte access, correct regardless of host
 * endianness. */
static inline uint16_t swap16(uint16_t x)
{
    return (uint16_t)((x << 8) | (x >> 8));
}

static inline uint32_t swap32(uint32_t x)
{
    return ((x & 0x000000ffUL) << 24) | ((x & 0x0000ff00UL) << 8) |
           ((x & 0x00ff0000UL) >> 8)  | ((x & 0xff000000UL) >> 24);
}

#endif /* MINIAMP3_WMA_PLATFORM_H */
