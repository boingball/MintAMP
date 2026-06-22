/*
 * flac_module.c - FLAC decoder module for MiniAMP3 using libfoxenflac.
 *
 * Clone libfoxenflac into decoders/flac/ before building:
 *   git submodule add https://github.com/astoeckel/libfoxenflac decoders/flac
 *
 * The module implements the DecoderOps vtable declared in decoder_module.h.
 * All memory is allocated with exec AllocMem/FreeVec (MEMF_FAST only) so
 * the decoder never touches chip RAM.
 *
 * Output: interleaved signed 16-bit PCM (L0 R0 L1 R1 ...).
 * 24-bit FLAC is down-shifted to 16 bits automatically.
 */

#include "../decoder_module.h"

/*
 * libfoxenflac header.  The library uses a "user-provided memory" pattern:
 *   sz = fxflac_size();            -- how many bytes to allocate
 *   fxflac_init(ptr);              -- initialise (after zeroing)
 *   fxflac_feed(ptr, &buf, &len); -- returns FXFLAC_OK / FXFLAC_BLOCK_DECODED
 *   fxflac_read(ptr, chans, &n);  -- get int32_t** channels after decode
 *   fxflac_get_streaminfo(ptr, &si); -- sample_rate, n_channels, sample_size
 *
 * Adjust the include path if the header lives elsewhere in the submodule.
 */
#include "flac/foxen_flac.h"

#ifdef HAVE_AMIGA_AUDIO_DEVICE
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>

extern struct ExecBase *SysBase;  /* set by DecoderModuleEntry in flac_entry.c */

static void *ModuleAlloc(unsigned long bytes)
{
    return AllocMem(bytes, MEMF_FAST | MEMF_CLEAR);
}

static void ModuleFree(void *ptr, unsigned long bytes)
{
    if (ptr)
        FreeMem(ptr, bytes);
}
#else
/* Host-build stubs (for cross-compile tests on Linux etc.) */
#include <stdlib.h>
#include <string.h>
static void *ModuleAlloc(unsigned long bytes)
{
    void *p = malloc((size_t)bytes);
    if (p) memset(p, 0, (size_t)bytes);
    return p;
}
static void ModuleFree(void *ptr, unsigned long bytes)
{
    (void)bytes;
    free(ptr);
}
#endif

/* --- I/O buffer size ---------------------------------------------------- */

#define FLAC_IO_BUF  (16 * 1024UL)  /* compressed-data feed buffer (Fast RAM) */

/* --- Per-stream state ---------------------------------------------------- */

typedef struct FlacState {
    /* libfoxenflac state — allocated as fxflac_size() bytes */
    FxFlac               *flac;
    unsigned long         flacSize;

    /* Compressed-data I/O buffer */
    unsigned char        *iobuf;
    unsigned long         iobufFill;   /* valid bytes currently in iobuf   */
    const unsigned char  *iobufPtr;    /* current read position in iobuf   */

    /* Current decoded block (int32_t** into FxFlac state, valid until next feed) */
    int32_t             **chans;
    unsigned long         blockSamples;   /* total samples per channel in block */
    unsigned long         blockConsumed;  /* samples per channel already returned */

    /* Stream parameters */
    int                   channels;
    int                   sampleSize;  /* bits per sample (16 or 24) */
    int                   shift;       /* right-shift to reach 16-bit range */

    /* I/O callbacks */
    DecoderReadCb         readFn;
    DecoderSeekCb         seekFn;
    void                 *userData;

    int                   eof;
    int                   error;
} FlacState;

/* --- Helper: refill the compressed-data buffer -------------------------- */

static int FlacRefillIoBuf(FlacState *st)
{
    DecLong got;

    if (st->eof)
        return 0;
    got = st->readFn(st->userData, st->iobuf, FLAC_IO_BUF);
    if (got <= 0) {
        st->eof = 1;
        st->iobufFill = 0;
        st->iobufPtr  = st->iobuf;
        return 0;
    }
    st->iobufFill = (unsigned long)got;
    st->iobufPtr  = st->iobuf;
    return 1;
}

/* --- Helper: feed until a block is decoded or EOF/error ----------------- */

static int FlacDecodeNextBlock(FlacState *st)
{
    FxFlacStatus status;
    uint32_t     n_samples = 0;

    st->blockSamples  = 0;
    st->blockConsumed = 0;

    for (;;) {
        if (st->iobufFill == 0) {
            if (!FlacRefillIoBuf(st))
                return 0;  /* EOF */
        }

        {
            uint32_t remaining = (uint32_t)st->iobufFill;
            status = fxflac_feed(st->flac,
                                 (const uint8_t **)&st->iobufPtr,
                                 &remaining);
            st->iobufFill = remaining;
        }

        if (status == FXFLAC_BLOCK_DECODED) {
            fxflac_read(st->flac, &st->chans, &n_samples);
            st->blockSamples  = (unsigned long)n_samples;
            st->blockConsumed = 0;
            return 1;
        }
        if (status == FXFLAC_OK) {
            /* Decoder wants more input; iobufFill should now be 0 */
            continue;
        }
        /* Error or end-of-stream */
        st->error = 1;
        return 0;
    }
}

/* --- DecoderOps callbacks ----------------------------------------------- */

static DecHandle FlacOpen(DecoderReadCb readFn, DecoderSeekCb seekFn,
                          void *userData, struct DecoderStreamInfo *infoOut)
{
    FlacState           *st;
    FxFlacStreamInfo     si;
    unsigned long        flacSize;
    int                  gotInfo = 0;
    int                  tries;

    flacSize = (unsigned long)fxflac_size();

    st = (FlacState *)ModuleAlloc(sizeof(FlacState));
    if (!st)
        return NULL;

    st->flac = (FxFlac *)ModuleAlloc(flacSize);
    if (!st->flac) {
        ModuleFree(st, sizeof(FlacState));
        return NULL;
    }
    st->flacSize = flacSize;

    st->iobuf = (unsigned char *)ModuleAlloc(FLAC_IO_BUF);
    if (!st->iobuf) {
        ModuleFree(st->flac, flacSize);
        ModuleFree(st, sizeof(FlacState));
        return NULL;
    }

    fxflac_init(st->flac);
    st->readFn   = readFn;
    st->seekFn   = seekFn;
    st->userData = userData;

    /* Feed enough data to obtain STREAMINFO (usually within the first block) */
    for (tries = 0; tries < 256 && !gotInfo; tries++) {
        if (!FlacDecodeNextBlock(st) && st->error)
            break;
        /* Check if streaminfo is available now */
        fxflac_get_streaminfo(st->flac, &si);
        if (si.sample_rate > 0)
            gotInfo = 1;
        /* If a real audio block decoded on the first try we have info too */
        if (st->blockSamples > 0)
            gotInfo = 1;
    }

    if (!gotInfo || si.sample_rate == 0) {
        ModuleFree(st->iobuf, FLAC_IO_BUF);
        ModuleFree(st->flac,  flacSize);
        ModuleFree(st,        sizeof(FlacState));
        return NULL;
    }

    st->channels   = (int)si.n_channels;
    st->sampleSize = (int)si.sample_size;
    st->shift      = st->sampleSize > 16 ? (st->sampleSize - 16) : 0;

    if (st->channels < 1) st->channels = 1;
    if (st->channels > 2) st->channels = 2;

    infoOut->sampleRate    = si.sample_rate;
    infoOut->channels      = (unsigned short)st->channels;
    infoOut->bitsPerSample = (unsigned short)st->sampleSize;
    infoOut->totalSamples  = (DecULong)si.n_samples;

    return (DecHandle)st;
}

static DecLong FlacDecode(DecHandle handle, short *outBuf,
                          DecULong maxSamplesPerChan)
{
    FlacState    *st = (FlacState *)handle;
    DecULong      produced = 0;
    int           ch;

    if (!st || st->error)
        return -1;

    while (produced < maxSamplesPerChan) {
        DecULong avail;
        DecULong take;
        DecULong s;

        /* Decode a new block if the current one is exhausted */
        if (st->blockConsumed >= st->blockSamples) {
            if (!FlacDecodeNextBlock(st)) {
                /* EOF or error */
                break;
            }
            if (st->blockSamples == 0)
                break;  /* EOF without a new audio block */
        }

        avail = st->blockSamples - st->blockConsumed;
        take  = avail;
        if (take > maxSamplesPerChan - produced)
            take = maxSamplesPerChan - produced;

        /* Convert int32_t → int16_t and interleave channels */
        for (s = 0; s < take; s++) {
            DecULong src = st->blockConsumed + s;
            DecULong dst = (produced + s) * (DecULong)st->channels;

            for (ch = 0; ch < st->channels; ch++) {
                int32_t raw = st->chans[ch][src];
                short   s16 = (short)(raw >> st->shift);
                outBuf[dst + (DecULong)ch] = s16;
            }
        }

        st->blockConsumed += take;
        produced          += take;
    }

    return (DecLong)produced;
}

static DecLong FlacSeek(DecHandle handle, DecULong samplePos)
{
    /* Basic implementation: rewind and skip.
     * A real seek would use the FLAC seektable via the seek callback.
     * For now, return non-zero to indicate unsupported (stream will restart). */
    (void)handle;
    (void)samplePos;
    return -1;
}

static void FlacClose(DecHandle handle)
{
    FlacState *st = (FlacState *)handle;

    if (!st)
        return;
    ModuleFree(st->iobuf, FLAC_IO_BUF);
    ModuleFree(st->flac,  st->flacSize);
    ModuleFree(st,        sizeof(FlacState));
}

static DecLong FlacGetTag(DecHandle handle, DecULong tagId,
                          void *valueOut, DecULong maxBytes)
{
    (void)handle;
    (void)tagId;
    (void)valueOut;
    (void)maxBytes;
    return -1;
}

/* --- Module info --------------------------------------------------------- */

static struct DecoderModuleInfo gFlacInfo = {
    DECODER_MODULE_MAGIC,
    DECODER_MODULE_VERSION,
    0,          /* revision */
    "FLAC",
    "flac\0fla\0\0",
    DECF_NEEDS_SEEK
};

/* gFlacOps is referenced by flac_entry.c */
struct DecoderOps gFlacOps = {
    &gFlacInfo,
    FlacOpen,
    FlacDecode,
    FlacSeek,
    FlacClose,
    FlacGetTag
};
