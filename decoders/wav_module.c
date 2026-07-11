/*
 * wav_module.c - WAV (RIFF/PCM) decoder module for MiniAMP3.
 *
 * Implements the DecoderOps vtable from decoder_module.h for plain
 * uncompressed PCM WAV files: 8/16/24/32-bit integer samples, mono or
 * stereo, both the classic "fmt " (format tag 1) and WAVE_FORMAT_EXTENSIBLE
 * (0xFFFE) headers used by many modern encoders for >16-bit/>2-channel
 * files (only the PCM sub-format is accepted; IEEE float WAVs are
 * rejected at open()).
 *
 * There is no third-party library here -- this is a small hand-written
 * RIFF chunk walker -- so unlike flac_module.c/aac_module.c/ogg_module.c
 * there is no forced malloc/calloc/free shim: wav_alloc.c exposes its own
 * WavModule* names and this file calls them directly.
 */

#include "decoder_module.h"
#include "wav_alloc.h"

#define WAV_MAX_CHANNELS 2U
#define WAV_IO_CAP       (16UL * 1024UL)
#define WAV_MODULE_BUILD_ID "WAV MODULE BUILD MARKER 12345 rev 1"

typedef struct WavState {
    DecoderReadCb readFn;
    DecoderSeekCb seekFn;
    void         *userData;

    unsigned char *iobuf;
    unsigned long  iobufFill;  /* valid bytes in iobuf                     */
    unsigned long  iobufPos;   /* read cursor into iobuf                   */

    unsigned long  dataChunkStart; /* absolute byte offset of PCM payload  */
    unsigned long  dataChunkSize;  /* total PCM payload bytes (immutable)  */
    unsigned long  dataRemaining;  /* PCM bytes not yet pulled from readFn */

    unsigned long  frameSize;      /* channels * bytesPerSample            */
    unsigned long  bytesPerSample; /* 1, 2, 3 or 4                         */
    int            channels;

    int eof;
    int error;
} WavState;

static unsigned short WavLE16(const unsigned char *p)
{
    return (unsigned short)((unsigned short)p[0] | ((unsigned short)p[1] << 8));
}

static unsigned long WavLE32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static void WavMemMove(unsigned char *dst, const unsigned char *src, unsigned long n)
{
    unsigned long i;
    /* Only ever called to shift a buffer's tail down toward index 0
     * (dst < src), so a plain ascending copy is safe. */
    for (i = 0; i < n; i++)
        dst[i] = src[i];
}

static int WavReadExactRaw(DecoderReadCb readFn, void *userData,
                            unsigned char *buf, unsigned long n)
{
    unsigned long got = 0;
    while (got < n) {
        DecLong r = readFn(userData, buf + got, n - got);
        if (r <= 0)
            return 0;
        got += (unsigned long)r;
    }
    return 1;
}

static int WavSkipRaw(DecoderReadCb readFn, void *userData, unsigned long n)
{
    unsigned char scratch[64];
    while (n > 0) {
        unsigned long want = (n > sizeof(scratch)) ? (unsigned long)sizeof(scratch) : n;
        DecLong r = readFn(userData, scratch, want);
        if (r <= 0)
            return 0;
        n -= (unsigned long)r;
    }
    return 1;
}

static short WavSampleToS16(const unsigned char *p, unsigned long bytesPerSample)
{
    switch (bytesPerSample) {
    case 1:
        /* WAV 8-bit PCM is unsigned, centred on 128. */
        return (short)(((int)p[0] - 128) << 8);
    case 2:
        return (short)((unsigned short)p[0] | ((unsigned short)p[1] << 8));
    case 3: {
        unsigned long u = (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
                           ((unsigned long)p[2] << 16);
        if (u & 0x00800000UL)
            u |= 0xFF000000UL; /* sign-extend 24 -> 32 bits */
        return (short)(((long)u) >> 8);
    }
    case 4: {
        unsigned long u = (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
                           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
        return (short)(((long)u) >> 16);
    }
    default:
        return 0;
    }
}

static int WavRefill(WavState *st)
{
    unsigned long left;

    if (st->iobufPos > 0) {
        left = st->iobufFill - st->iobufPos;
        if (left > 0)
            WavMemMove(st->iobuf, st->iobuf + st->iobufPos, left);
        st->iobufFill = left;
        st->iobufPos  = 0;
    }

    while (st->iobufFill < WAV_IO_CAP && st->dataRemaining > 0) {
        unsigned long want = WAV_IO_CAP - st->iobufFill;
        DecLong got;
        if (want > st->dataRemaining)
            want = st->dataRemaining;
        got = st->readFn(st->userData, st->iobuf + st->iobufFill, want);
        if (got <= 0) {
            st->eof = 1;
            break;
        }
        st->iobufFill     += (unsigned long)got;
        st->dataRemaining -= (unsigned long)got;
    }

    return st->iobufFill > st->iobufPos;
}

static DecHandle WavOpen(DecoderReadCb readFn, DecoderSeekCb seekFn,
                          void *userData, struct DecoderStreamInfo *infoOut)
{
    WavState     *st;
    unsigned char hdr[12];
    unsigned char chunkHdr[8];
    unsigned long pos = 0;
    int           haveFmt  = 0;
    int           haveData = 0;
    unsigned short audioFormat   = 0;
    unsigned short channels      = 0;
    unsigned long  sampleRate    = 0;
    unsigned short bitsPerSample = 0;

    st = (WavState *)WavModuleCalloc(1, sizeof(WavState));
    if (!st)
        return NULL;

    st->readFn   = readFn;
    st->seekFn   = seekFn;
    st->userData = userData;

    if (!WavReadExactRaw(readFn, userData, hdr, 12))
        goto fail;
    pos += 12;
    if (hdr[0] != 'R' || hdr[1] != 'I' || hdr[2] != 'F' || hdr[3] != 'F')
        goto fail;
    if (hdr[8] != 'W' || hdr[9] != 'A' || hdr[10] != 'V' || hdr[11] != 'E')
        goto fail;

    for (;;) {
        unsigned long chunkSize;

        if (!WavReadExactRaw(readFn, userData, chunkHdr, 8))
            break; /* EOF before a 'data' chunk turned up */
        pos += 8;
        chunkSize = WavLE32(chunkHdr + 4);

        if (chunkHdr[0] == 'f' && chunkHdr[1] == 'm' && chunkHdr[2] == 't' &&
            chunkHdr[3] == ' ') {
            unsigned char fmtBuf[40];
            unsigned long readN = chunkSize;
            unsigned long remainder;

            if (readN > sizeof(fmtBuf))
                readN = sizeof(fmtBuf);
            if (readN < 16)
                goto fail;
            if (!WavReadExactRaw(readFn, userData, fmtBuf, readN))
                goto fail;
            pos += readN;

            remainder = chunkSize - readN;
            if (chunkSize & 1UL)
                remainder += 1; /* RIFF chunks are word-aligned */
            if (remainder && !WavSkipRaw(readFn, userData, remainder))
                goto fail;
            pos += remainder;

            audioFormat   = WavLE16(fmtBuf + 0);
            channels      = WavLE16(fmtBuf + 2);
            sampleRate    = WavLE32(fmtBuf + 4);
            bitsPerSample = WavLE16(fmtBuf + 14);

            /* WAVE_FORMAT_EXTENSIBLE: real format lives in the first two
             * bytes of the 16-byte SubFormat GUID at offset 24. */
            if (audioFormat == 0xFFFEU && readN >= 40)
                audioFormat = WavLE16(fmtBuf + 24);

            haveFmt = 1;
            continue;
        }

        if (chunkHdr[0] == 'd' && chunkHdr[1] == 'a' && chunkHdr[2] == 't' &&
            chunkHdr[3] == 'a') {
            if (!haveFmt)
                goto fail; /* 'data' must be preceded by 'fmt ' */
            st->dataChunkStart = pos;
            st->dataChunkSize  = chunkSize;
            st->dataRemaining  = chunkSize;
            haveData = 1;
            break; /* stream cursor is positioned at the first PCM byte */
        }

        /* Uninteresting chunk ('fact', 'LIST', 'smpl', 'cue ', ...) */
        {
            unsigned long skip = chunkSize + (chunkSize & 1UL);
            if (skip && !WavSkipRaw(readFn, userData, skip))
                goto fail;
            pos += skip;
        }
    }

    if (!haveFmt || !haveData)
        goto fail;
    if (audioFormat != 1) /* only plain integer PCM is supported */
        goto fail;
    if (channels < 1 || channels > WAV_MAX_CHANNELS)
        goto fail;
    if (sampleRate == 0)
        goto fail;
    if (bitsPerSample != 8 && bitsPerSample != 16 &&
        bitsPerSample != 24 && bitsPerSample != 32)
        goto fail;

    st->bytesPerSample = (unsigned long)bitsPerSample / 8UL;
    st->channels        = (int)channels;
    st->frameSize        = st->bytesPerSample * (unsigned long)channels;
    if (st->frameSize == 0)
        goto fail;

    st->iobuf = (unsigned char *)WavModuleCalloc(1, WAV_IO_CAP);
    if (!st->iobuf)
        goto fail;

    infoOut->sampleRate    = (DecULong)sampleRate;
    infoOut->channels      = channels;
    infoOut->bitsPerSample = bitsPerSample;
    infoOut->totalSamples  = (DecULong)(st->dataChunkSize / st->frameSize);

    return (DecHandle)st;

fail:
    if (st) {
        if (st->iobuf)
            WavModuleFree(st->iobuf);
        WavModuleFree(st);
    }
    return NULL;
}

static DecLong WavDecode(DecHandle handle, short *outBuf, DecULong maxSamplesPerChan)
{
    WavState     *st = (WavState *)handle;
    unsigned long produced = 0;
    unsigned long ch;

    if (!st)
        return -1;
    if (st->error)
        return -1;
    if (maxSamplesPerChan == 0)
        return 0;

    ch = (unsigned long)st->channels;

    while (produced < (unsigned long)maxSamplesPerChan) {
        unsigned long avail = st->iobufFill - st->iobufPos;
        unsigned long framesAvail, framesWant, take, f, i;
        const unsigned char *src;
        short *dst;

        if (avail < st->frameSize) {
            if (!WavRefill(st))
                break;
            avail = st->iobufFill - st->iobufPos;
            if (avail < st->frameSize)
                break; /* trailing partial frame / EOF -- stop cleanly */
        }

        framesAvail = avail / st->frameSize;
        framesWant  = (unsigned long)maxSamplesPerChan - produced;
        take        = (framesAvail < framesWant) ? framesAvail : framesWant;
        if (take == 0)
            break;

        src = st->iobuf + st->iobufPos;
        dst = outBuf + produced * ch;

        for (f = 0; f < take; f++)
            for (i = 0; i < ch; i++)
                dst[f * ch + i] =
                    WavSampleToS16(src + (f * ch + i) * st->bytesPerSample,
                                   st->bytesPerSample);

        st->iobufPos += take * st->frameSize;
        produced     += take;
    }

    return (DecLong)produced;
}

static DecLong WavSeek(DecHandle handle, DecULong samplePos)
{
    WavState     *st = (WavState *)handle;
    unsigned long bytePos;
    unsigned long absPos;
    DecLong       rc;

    if (!st || !st->seekFn)
        return -1;

    bytePos = (unsigned long)samplePos * st->frameSize;
    if (bytePos > st->dataChunkSize)
        bytePos = st->dataChunkSize;

    absPos = st->dataChunkStart + bytePos;
    rc = st->seekFn(st->userData, (DecLong)absPos, 0 /* SEEK_SET */);
    if (rc != 0)
        return -1;

    st->iobufFill     = 0;
    st->iobufPos      = 0;
    st->dataRemaining = st->dataChunkSize - bytePos;
    st->eof   = 0;
    st->error = 0;
    return 0;
}

static DecLong WavGetIoHints(DecHandle handle, struct DecoderIoHints *out)
{
    (void)handle;
    if (!out)
        return -1;
    out->preferred_read_bytes = WAV_IO_CAP;
    out->prefetch_bytes       = WAV_IO_CAP * 2UL;
    return 0;
}

static void WavClose(DecHandle handle)
{
    WavState *st = (WavState *)handle;
    if (!st)
        return;
    if (st->iobuf)
        WavModuleFree(st->iobuf);
    WavModuleFree(st);
}

static struct DecoderModuleInfo gWavInfo = {
    DECODER_MODULE_MAGIC,
    DECODER_MODULE_VERSION,
    0,
    WAV_MODULE_BUILD_ID,
    "wav\0wave\0",
    0
};

struct DecoderOps gWavOps = {
    &gWavInfo,
    WavOpen,
    WavDecode,
    WavSeek,
    WavClose,
    NULL,           /* get_tag */
    WavGetIoHints
};
