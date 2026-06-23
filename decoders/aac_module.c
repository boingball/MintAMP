/*
 * aac_module.c - AAC decoder module for MiniAMP3 using libhelix-aac.
 *
 * Implements the DecoderOps vtable from decoder_module.h.
 * Targets ADTS-framed AAC (.aac files); raw/ADIF also handled by the
 * underlying libhelix-aac if the sync-word scan succeeds.
 *
 * Memory footprint (no SBR): ~20 KB Fast RAM total
 *   AACDecInfo + PSInfoBase  ~12 KB  (heap via AacModuleMalloc)
 *   I/O buffer               ~8  KB
 *   PCM output buffer        ~4  KB
 *   Module state             <1  KB
 */

#include "decoder_module.h"
#include "aac_alloc.h"
#include "aac/aacdec.h"

#define AAC_MODULE_BUILD_ID "AAC MODULE BUILD MARKER 12345 rev 1"

/* Compressed input ring buffer.  Must hold at least two maximum ADTS frames
 * (AAC_MAINBUF_SIZE = 768*2 = 1536 bytes each) to guarantee AACDecode always
 * has a full frame available.  8 KB amortises Exec I/O calls nicely. */
#define AAC_IO_CAP  (8UL * 1024UL)

/* PCM output: one decoded frame (1024 samples * 2 channels max).
 * SBR doubles to 2048 samples/ch, but SBR is disabled in this build. */
#define AAC_OUT_CAP  (1024UL * 2UL)

/* Refill the I/O buffer when fewer than this many bytes remain.
 * AAC_MAINBUF_SIZE guarantees at least one full frame fits after refill. */
#define AAC_REFILL_THRESH  ((unsigned long)AAC_MAINBUF_SIZE)

/* Give up on a stream after this many consecutive frame errors. */
#define AAC_MAX_ERRORS  64

static void *ModuleAlloc(unsigned long bytes)
{
    return AacModuleCalloc(1, (size_t)bytes);
}

static void ModuleFree(void *ptr)
{
    AacModuleFree(ptr);
}

/* Byte-safe memmove — needed when sliding unread data to the buffer front. */
static void AacMoveMem(unsigned char *dst, const unsigned char *src,
                       unsigned long n)
{
    if (dst == src || n == 0) return;
    if (dst < src) {
        while (n--) *dst++ = *src++;
    } else {
        dst += n; src += n;
        while (n--) *--dst = *--src;
    }
}

typedef struct AacState {
    HAACDecoder    aacHandle;

    /* Sliding I/O buffer */
    unsigned char *iobuf;        /* base of buffer */
    unsigned long  iobufCap;     /* total capacity */
    unsigned char *iobufReadPtr; /* current read position within iobuf */
    int            iobufLeft;    /* bytes available from iobufReadPtr */

    /* Decoded PCM (interleaved signed 16-bit, host byte order) */
    short         *outbuf;
    unsigned long  outbufFill;   /* valid interleaved shorts */
    unsigned long  outbufPos;    /* read cursor in shorts */

    int  channels;
    int  errCount;

    DecoderReadCb  readFn;
    DecoderSeekCb  seekFn;
    void          *userData;

    int  eof;
    int  error;
    int  done;
} AacState;

static void AacFreeState(AacState *st)
{
    if (!st) return;
    if (st->aacHandle) AACFreeDecoder(st->aacHandle);
    if (st->outbuf)    ModuleFree(st->outbuf);
    if (st->iobuf)     ModuleFree(st->iobuf);
    ModuleFree(st);
}

/*
 * Slide remaining bytes to the front of iobuf and read more data from the
 * stream.  Returns 1 if data is available, 0 if EOF with empty buffer.
 */
static int AacRefillBuf(AacState *st)
{
    DecLong got;
    unsigned long space;

    if (st->iobufLeft > 0 && st->iobufReadPtr != st->iobuf)
        AacMoveMem(st->iobuf, st->iobufReadPtr, (unsigned long)st->iobufLeft);
    st->iobufReadPtr = st->iobuf;

    if (!st->eof) {
        space = st->iobufCap - (unsigned long)st->iobufLeft;
        got = st->readFn(st->userData, st->iobuf + st->iobufLeft, space);
        if (got <= 0)
            st->eof = 1;
        else
            st->iobufLeft += (int)got;
    }

    return (st->iobufLeft > 0);
}

/*
 * Decode exactly one AAC frame into st->outbuf.
 * Returns 1 with PCM ready, 0 for EOF, -1 for a recoverable frame error.
 */
static int AacDecodeFrame(AacState *st)
{
    int offset, err;
    AACFrameInfo fi;

    /* Ensure buffer has room for a full frame */
    if ((unsigned long)st->iobufLeft < AAC_REFILL_THRESH)
        AacRefillBuf(st);

    if (st->iobufLeft <= 0) {
        st->done = 1;
        return 0;
    }

    /* Locate next ADTS sync word (0xFFF) */
    offset = AACFindSyncWord(st->iobufReadPtr, st->iobufLeft);
    if (offset < 0) {
        if (st->eof) { st->done = 1; return 0; }
        AacRefillBuf(st);
        offset = AACFindSyncWord(st->iobufReadPtr, st->iobufLeft);
        if (offset < 0) {
            /* No sync in entire buffer — stream is corrupt or ended */
            st->iobufLeft = 0;
            return -1;
        }
    }
    st->iobufReadPtr += offset;
    st->iobufLeft    -= offset;

    err = AACDecode(st->aacHandle,
                    &st->iobufReadPtr,
                    &st->iobufLeft,
                    st->outbuf);
    if (err != 0) {
        /* Step past the failed sync and let the caller retry */
        if (st->iobufLeft > 0) {
            st->iobufReadPtr++;
            st->iobufLeft--;
        }
        return -1;
    }

    AACGetLastFrameInfo(st->aacHandle, &fi);
    if (fi.outputSamps <= 0 || fi.nChans != st->channels)
        return -1;

    st->outbufFill = (unsigned long)fi.outputSamps;
    st->outbufPos  = 0;
    return 1;
}

static DecHandle AacOpen(DecoderReadCb readFn, DecoderSeekCb seekFn,
                         void *userData, struct DecoderStreamInfo *infoOut)
{
    AacState    *st;
    AACFrameInfo fi;
    int          offset, err;

    st = (AacState *)ModuleAlloc(sizeof(AacState));
    if (!st) return NULL;

    st->iobuf = (unsigned char *)ModuleAlloc(AAC_IO_CAP);
    if (!st->iobuf) { AacFreeState(st); return NULL; }
    st->iobufCap     = AAC_IO_CAP;
    st->iobufReadPtr = st->iobuf;
    st->iobufLeft    = 0;

    st->outbuf = (short *)ModuleAlloc(AAC_OUT_CAP * sizeof(short));
    if (!st->outbuf) { AacFreeState(st); return NULL; }

    st->readFn   = readFn;
    st->seekFn   = seekFn;
    st->userData = userData;

    /* AACInitDecoder() allocates its internal state via malloc()
     * which is remapped to AacModuleMalloc through the -D flags. */
    st->aacHandle = AACInitDecoder();
    if (!st->aacHandle) { AacFreeState(st); return NULL; }

    /* Prime buffer and find first sync word */
    if (!AacRefillBuf(st)) { AacFreeState(st); return NULL; }

    offset = AACFindSyncWord(st->iobufReadPtr, st->iobufLeft);
    if (offset < 0) { AacFreeState(st); return NULL; }
    st->iobufReadPtr += offset;
    st->iobufLeft    -= offset;

    /* Decode first frame — probes sample rate, channel count, bit depth */
    err = AACDecode(st->aacHandle,
                    &st->iobufReadPtr,
                    &st->iobufLeft,
                    st->outbuf);
    if (err != 0) { AacFreeState(st); return NULL; }

    AACGetLastFrameInfo(st->aacHandle, &fi);
    if (fi.nChans <= 0 || fi.sampRateOut <= 0 ||
        fi.nChans > 2  || fi.outputSamps <= 0) {
        AacFreeState(st); return NULL;
    }

    st->channels   = fi.nChans;
    st->outbufFill = (unsigned long)fi.outputSamps;
    st->outbufPos  = 0;

    infoOut->sampleRate    = (DecULong)fi.sampRateOut;
    infoOut->channels      = (unsigned short)fi.nChans;
    infoOut->bitsPerSample = (unsigned short)(fi.bitsPerSample > 0 ?
                                              fi.bitsPerSample : 16);
    infoOut->totalSamples  = 0; /* unknown for streaming ADTS */

    return (DecHandle)st;
}

static DecLong AacDecode(DecHandle handle, short *outBuf,
                          DecULong maxSamplesPerChan)
{
    AacState    *st = (AacState *)handle;
    DecULong     produced = 0;
    unsigned long ch;

    if (!st)       return -1;
    if (st->error) return -1;
    if (st->done)  return 0;

    ch = (unsigned long)st->channels;

    while (produced < maxSamplesPerChan) {
        /* Drain PCM left from the previous decode call */
        if (st->outbufPos < st->outbufFill) {
            unsigned long avail = (st->outbufFill - st->outbufPos) / ch;
            unsigned long take = (unsigned long)(maxSamplesPerChan - produced);
            unsigned long i, total;
            if (take > avail) take = avail;
            total = take * ch;
            {
                const short *src = st->outbuf + st->outbufPos;
                short *dst = outBuf + produced * ch;
                for (i = 0; i < total; i++)
                    dst[i] = src[i];
            }
            st->outbufPos += take * ch;
            produced      += take;
            st->errCount   = 0;
            continue;
        }

        /* Decode the next compressed frame */
        {
            int rc = AacDecodeFrame(st);
            if (rc > 0) {
                st->errCount = 0;
                continue;
            }
            if (rc == 0) break; /* EOF */
            /* Recoverable frame error — skip and retry up to limit */
            if (++st->errCount > AAC_MAX_ERRORS) {
                st->error = 1;
                return -1;
            }
        }
    }

    if (st->error) return -1;
    return (DecLong)produced;
}

static DecLong AacSeek(DecHandle handle, DecULong samplePos)
{
    (void)handle; (void)samplePos;
    return -1;
}

static void AacClose(DecHandle handle)
{
    AacFreeState((AacState *)handle);
}

static struct DecoderModuleInfo gAacInfo = {
    DECODER_MODULE_MAGIC,
    DECODER_MODULE_VERSION,
    0,
    AAC_MODULE_BUILD_ID,
    "aac\0",
    0
};

struct DecoderOps gAacOps = {
    &gAacInfo,
    AacOpen,
    AacDecode,
    AacSeek,
    AacClose,
    NULL
};
