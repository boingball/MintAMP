/*
 * iff_module.c - Amiga IFF-8SVX decoder module for MiniAMP3.
 *
 * Implements the DecoderOps vtable from decoder_module.h for the classic
 * Amiga "8-bit Voice" sample format: FORM/8SVX/VHDR/BODY, mono, 8-bit
 * signed samples, sCompression 0 (raw) or 1 (Fibonacci-delta).  IFF chunk
 * sizes are big-endian, unlike WAV's little-endian RIFF sizes.
 *
 * No third-party library here either -- like wav_module.c this is a small
 * hand-written chunk walker, so iff_alloc.c exposes its own IffModule*
 * names directly instead of shadowing malloc/calloc/free.
 */

#include "decoder_module.h"
#include "iff_alloc.h"

#define IFF_IO_CAP (8UL * 1024UL)
#define IFF_MODULE_BUILD_ID "IFF MODULE BUILD MARKER 12345 rev 1"

/* IFF-8SVX Fibonacci-delta codebook (sCompression == 1). */
static const int gIffFib[16] = {
    -34, -21, -13, -8, -5, -3, -2, -1, 0, 1, 2, 3, 5, 8, 13, 21
};

typedef struct IffState {
    DecoderReadCb readFn;
    DecoderSeekCb seekFn;
    void         *userData;

    unsigned char *iobuf;
    unsigned long  iobufFill;
    unsigned long  iobufPos;

    unsigned long  bodyChunkStart; /* absolute byte offset of BODY payload */
    unsigned long  bodyChunkSize;  /* total BODY payload bytes (immutable) */
    unsigned long  dataRemaining;  /* BODY bytes not yet pulled from readFn */

    int compression; /* 0 = raw, 1 = Fibonacci delta */

    /* Fibonacci-delta streaming state */
    int       seeded;
    signed char prevSample;
    int       havePending;
    signed char pendingRaw;

    int eof;
    int error;
} IffState;

static unsigned short IffBE16(const unsigned char *p)
{
    return (unsigned short)(((unsigned short)p[0] << 8) | (unsigned short)p[1]);
}

static unsigned long IffBE32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8)  |  (unsigned long)p[3];
}

static int IffReadExactRaw(DecoderReadCb readFn, void *userData,
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

static int IffSkipRaw(DecoderReadCb readFn, void *userData, unsigned long n)
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

static short IffSampleToS16(signed char s)
{
    return (short)((int)s * 256);
}

static signed char IffFibClamp(int v)
{
    if (v > 127)
        v = 127;
    else if (v < -128)
        v = -128;
    return (signed char)v;
}

static void IffMemMove(unsigned char *dst, const unsigned char *src, unsigned long n)
{
    unsigned long i;
    for (i = 0; i < n; i++)
        dst[i] = src[i];
}

static int IffRefill(IffState *st)
{
    unsigned long left;

    if (st->iobufPos > 0) {
        left = st->iobufFill - st->iobufPos;
        if (left > 0)
            IffMemMove(st->iobuf, st->iobuf + st->iobufPos, left);
        st->iobufFill = left;
        st->iobufPos  = 0;
    }

    while (st->iobufFill < IFF_IO_CAP && st->dataRemaining > 0) {
        unsigned long want = IFF_IO_CAP - st->iobufFill;
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

static int IffEnsureByte(IffState *st)
{
    if (st->iobufPos < st->iobufFill)
        return 1;
    if (!IffRefill(st))
        return 0;
    return st->iobufPos < st->iobufFill;
}

static DecHandle IffOpen(DecoderReadCb readFn, DecoderSeekCb seekFn,
                          void *userData, struct DecoderStreamInfo *infoOut)
{
    IffState     *st;
    unsigned char hdr[12];
    unsigned char chunkHdr[8];
    unsigned long pos = 0;
    int            haveVhdr = 0;
    int            haveBody = 0;
    unsigned long  sampleRate = 0;
    int            compression = -1;

    st = (IffState *)IffModuleCalloc(1, sizeof(IffState));
    if (!st)
        return NULL;

    st->readFn   = readFn;
    st->seekFn   = seekFn;
    st->userData = userData;

    if (!IffReadExactRaw(readFn, userData, hdr, 12))
        goto fail;
    pos += 12;
    if (hdr[0] != 'F' || hdr[1] != 'O' || hdr[2] != 'R' || hdr[3] != 'M')
        goto fail;
    if (hdr[8] != '8' || hdr[9] != 'S' || hdr[10] != 'V' || hdr[11] != 'X')
        goto fail;

    for (;;) {
        unsigned long chunkSize;

        if (!IffReadExactRaw(readFn, userData, chunkHdr, 8))
            break; /* EOF before a BODY chunk turned up */
        pos += 8;
        chunkSize = IffBE32(chunkHdr + 4);

        if (chunkHdr[0] == 'V' && chunkHdr[1] == 'H' && chunkHdr[2] == 'D' &&
            chunkHdr[3] == 'R') {
            unsigned char vhdr[20];
            unsigned long readN = chunkSize;
            unsigned long remainder;

            if (readN > sizeof(vhdr))
                readN = sizeof(vhdr);
            /* Standard VHDR is exactly 20 bytes: oneShotHiSamples(4)
             * repeatHiSamples(4) samplesPerHiCycle(4) samplesPerSec(2)
             * ctOctave(1) sCompression(1) volume(4). Require the full
             * layout so sCompression at offset 15 is never misread. */
            if (readN < sizeof(vhdr))
                goto fail;
            if (!IffReadExactRaw(readFn, userData, vhdr, readN))
                goto fail;
            pos += readN;

            remainder = chunkSize - readN;
            if (chunkSize & 1UL)
                remainder += 1; /* IFF chunks are word-aligned */
            if (remainder && !IffSkipRaw(readFn, userData, remainder))
                goto fail;
            pos += remainder;

            sampleRate  = (unsigned long)IffBE16(vhdr + 12);
            compression = (int)vhdr[15];
            haveVhdr = 1;
            continue;
        }

        if (chunkHdr[0] == 'B' && chunkHdr[1] == 'O' && chunkHdr[2] == 'D' &&
            chunkHdr[3] == 'Y') {
            if (!haveVhdr)
                goto fail; /* BODY must be preceded by VHDR */
            st->bodyChunkStart = pos;
            st->bodyChunkSize  = chunkSize;
            st->dataRemaining  = chunkSize;
            haveBody = 1;
            break; /* stream cursor is positioned at the first BODY byte */
        }

        /* Uninteresting chunk (NAME, "(c) ", ANNO, ...) */
        {
            unsigned long skip = chunkSize + (chunkSize & 1UL);
            if (skip && !IffSkipRaw(readFn, userData, skip))
                goto fail;
            pos += skip;
        }
    }

    if (!haveVhdr || !haveBody)
        goto fail;
    if (sampleRate == 0)
        goto fail;
    if (compression != 0 && compression != 1)
        goto fail; /* unsupported sCompression (e.g. exponential) */

    st->compression = compression;

    st->iobuf = (unsigned char *)IffModuleCalloc(1, IFF_IO_CAP);
    if (!st->iobuf)
        goto fail;

    infoOut->sampleRate    = (DecULong)sampleRate;
    infoOut->channels      = 1;
    infoOut->bitsPerSample = 8;
    infoOut->totalSamples  = (compression == 0)
        ? (DecULong)st->bodyChunkSize
        : (st->bodyChunkSize > 0 ? (DecULong)(2UL * (st->bodyChunkSize - 1UL) + 1UL) : 0);

    return (DecHandle)st;

fail:
    if (st) {
        if (st->iobuf)
            IffModuleFree(st->iobuf);
        IffModuleFree(st);
    }
    return NULL;
}

static DecLong IffDecode(DecHandle handle, short *outBuf, DecULong maxSamplesPerChan)
{
    IffState     *st = (IffState *)handle;
    unsigned long produced = 0;

    if (!st)
        return -1;
    if (st->error)
        return -1;
    if (maxSamplesPerChan == 0)
        return 0;

    while (produced < (unsigned long)maxSamplesPerChan) {
        if (st->havePending) {
            outBuf[produced++] = IffSampleToS16(st->pendingRaw);
            st->havePending = 0;
            continue;
        }

        if (st->compression == 0) {
            unsigned long avail = st->iobufFill - st->iobufPos;
            unsigned long want, i;
            const unsigned char *src;

            if (avail == 0) {
                if (!IffRefill(st))
                    break;
                avail = st->iobufFill - st->iobufPos;
                if (avail == 0)
                    break;
            }

            want = (unsigned long)maxSamplesPerChan - produced;
            if (want > avail)
                want = avail;

            src = st->iobuf + st->iobufPos;
            for (i = 0; i < want; i++)
                outBuf[produced + i] = IffSampleToS16((signed char)src[i]);

            st->iobufPos += want;
            produced     += want;
            continue;
        }

        /* compression == 1: Fibonacci delta */
        if (!st->seeded) {
            if (!IffEnsureByte(st))
                break;
            st->prevSample = (signed char)st->iobuf[st->iobufPos++];
            st->seeded = 1;
            outBuf[produced++] = IffSampleToS16(st->prevSample);
            continue;
        }
        {
            unsigned char d;
            signed char   s1, s2;
            int           n1, n2;

            if (!IffEnsureByte(st))
                break;
            d  = st->iobuf[st->iobufPos++];
            n1 = (d >> 4) & 0x0F;
            n2 = d & 0x0F;
            s1 = IffFibClamp((int)st->prevSample + gIffFib[n1]);
            s2 = IffFibClamp((int)s1 + gIffFib[n2]);
            st->prevSample = s2;

            outBuf[produced++] = IffSampleToS16(s1);
            if (produced < (unsigned long)maxSamplesPerChan) {
                outBuf[produced++] = IffSampleToS16(s2);
            } else {
                st->pendingRaw  = s2;
                st->havePending = 1;
            }
        }
    }

    return (DecLong)produced;
}

static DecLong IffSeek(DecHandle handle, DecULong samplePos)
{
    IffState     *st = (IffState *)handle;
    unsigned long bytePos, absPos;
    DecLong       rc;

    /* Fibonacci-delta is a running-state stream -- only raw (uncompressed)
     * bodies support random-access seeking. */
    if (!st || !st->seekFn || st->compression != 0)
        return -1;

    bytePos = (unsigned long)samplePos;
    if (bytePos > st->bodyChunkSize)
        bytePos = st->bodyChunkSize;

    absPos = st->bodyChunkStart + bytePos;
    rc = st->seekFn(st->userData, (DecLong)absPos, 0 /* SEEK_SET */);
    if (rc != 0)
        return -1;

    st->iobufFill     = 0;
    st->iobufPos      = 0;
    st->dataRemaining = st->bodyChunkSize - bytePos;
    st->eof   = 0;
    st->error = 0;
    return 0;
}

static DecLong IffGetIoHints(DecHandle handle, struct DecoderIoHints *out)
{
    (void)handle;
    if (!out)
        return -1;
    out->preferred_read_bytes = IFF_IO_CAP;
    out->prefetch_bytes       = IFF_IO_CAP * 2UL;
    return 0;
}

static void IffClose(DecHandle handle)
{
    IffState *st = (IffState *)handle;
    if (!st)
        return;
    if (st->iobuf)
        IffModuleFree(st->iobuf);
    IffModuleFree(st);
}

static struct DecoderModuleInfo gIffInfo = {
    DECODER_MODULE_MAGIC,
    DECODER_MODULE_VERSION,
    0,
    IFF_MODULE_BUILD_ID,
    "8svx\0iff\0svx\0",
    0
};

struct DecoderOps gIffOps = {
    &gIffInfo,
    IffOpen,
    IffDecode,
    IffSeek,
    IffClose,
    NULL,           /* get_tag */
    IffGetIoHints
};
