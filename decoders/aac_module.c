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

#define AAC_MODULE_BUILD_ID "AAC MODULE BUILD MARKER 12345 rev 3"

/* Compressed input ring buffer.  Must hold at least two maximum ADTS frames
 * (AAC_MAINBUF_SIZE = 768*2 = 1536 bytes each) to guarantee AACDecode always
 * has a full frame available.  8 KB amortises Exec I/O calls nicely. */
#define AAC_IO_CAP  (8UL * 1024UL)

/* PCM output: one decoded frame (1024 samples * 2 channels max).
 * SBR doubles to 2048 samples/ch, but SBR is disabled in this build. */
/* Leave margin for decoders that can report larger frames (for example SBR
 * builds producing 2048 samples/ch stereo).  Capacity is interleaved shorts. */
#define AAC_OUT_CAP  (4096UL * 2UL)

/* Refill the I/O buffer when fewer than this many bytes remain.
 * AAC_MAINBUF_SIZE guarantees at least one full frame fits after refill. */
#define AAC_REFILL_THRESH  ((unsigned long)AAC_MAINBUF_SIZE)

/* Give up on a stream after this many consecutive frame errors. */
#define AAC_MAX_ERRORS  64
#define AAC_SYNC_SEARCH_GUARD  4

#define AAC_ERR_INVALID_FRAME  (-1)


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
    unsigned char *outbufAlloc;
    unsigned long  outbufAllocBytes;
    unsigned long  outbufFill;   /* valid interleaved shorts */
    unsigned long  outbufPos;    /* read cursor in shorts */

    int  channels;
    int  errCount;
    unsigned long totalDecodeErrors;
    unsigned long totalResyncs;
    unsigned long totalInvalidFrames;

    DecoderReadCb  readFn;
    DecoderSeekCb  seekFn;
    void          *userData;

    int  eof;
    int  error;
    int  done;

    /* Debug/progress state visible in memory dumps without module-side printf. */
    unsigned long lastSyncOffset;
    unsigned long lastBytesBefore;
    unsigned long lastBytesAfter;
    unsigned long lastInputBefore;
    unsigned long lastInputAfter;
    unsigned long lastSamplesProduced;
    int           lastDecodeErr;
} AacState;

static int AacRefillBuf(AacState *st);
static void AacTracePoint(AacState *st, const char *point, long err,
                          const AACFrameInfo *fi, long decodeReturn);

#define AAC_GUARD_BYTES 32UL
#define AAC_GUARD_PATTERN 0xA5

static void AacFillGuard(unsigned char *p)
{
    unsigned long i;
    if (!p) return;
    for (i = 0; i < AAC_GUARD_BYTES; i++) p[i] = (unsigned char)AAC_GUARD_PATTERN;
}

static int AacCheckGuard(const unsigned char *p)
{
    unsigned long i;
    if (!p) return 0;
    for (i = 0; i < AAC_GUARD_BYTES; i++)
        if (p[i] != (unsigned char)AAC_GUARD_PATTERN)
            return 0;
    return 1;
}

static int AacCheckOutputCanary(AacState *st, const char *where)
{
    unsigned char *pre;
    unsigned char *post;
    (void)where;
    if (!st || !st->outbufAlloc) return 1;
    pre = st->outbufAlloc;
    post = st->outbufAlloc + AAC_GUARD_BYTES + (AAC_OUT_CAP * (unsigned long)sizeof(short));
    if (!AacCheckGuard(pre) || !AacCheckGuard(post)) {
        st->error = 1;
        AacTracePoint(st, where ? where : "AAC output canary corrupted", -999, 0, 0);
        return 0;
    }
    return 1;
}

static void AacInitOutputCanary(AacState *st)
{
    if (!st || !st->outbufAlloc) return;
    AacFillGuard(st->outbufAlloc);
    AacFillGuard(st->outbufAlloc + AAC_GUARD_BYTES + (AAC_OUT_CAP * (unsigned long)sizeof(short)));
}


#ifdef AAC_MODULE_DEBUG
#define AAC_TRACE_MARKER "AAC MODULE DEBUG TRACE"
#endif

#ifdef HAVE_AMIGA_AUDIO_DEVICE
static void *gAacTraceExec;
static void *gAacTraceDos;

static void *AacTraceOpenLibrary(const char *name, unsigned long version)
{
    register void *a6 __asm("a6") = gAacTraceExec;
    register const char *a1 __asm("a1") = name;
    register unsigned long d0 __asm("d0") = version;

    if (!gAacTraceExec)
        return 0;
    __asm volatile ("jsr a6@(-552:W)"
                    : "+r" (d0)
                    : "r" (a1), "r" (a6)
                    : "a0", "d1", "d2", "cc", "memory");
    return (void *)d0;
}

static long AacTraceOutput(void)
{
    register void *a6 __asm("a6") = gAacTraceDos;
    register long d0 __asm("d0");

    if (!gAacTraceDos)
        return 0;
    __asm volatile ("jsr a6@(-60:W)"
                    : "=r" (d0)
                    : "r" (a6)
                    : "a0", "a1", "d1", "d2", "cc", "memory");
    return d0;
}

static void AacTraceWrite(long fh, const char *buf, unsigned long len)
{
    register void *a6 __asm("a6") = gAacTraceDos;
    register long d1 __asm("d1") = fh;
    register const char *d2 __asm("d2") = buf;
    register unsigned long d3 __asm("d3") = len;

    if (!gAacTraceDos || !fh || !buf || len == 0)
        return;
    __asm volatile ("jsr a6@(-48:W)"
                    :
                    : "r" (d1), "r" (d2), "r" (d3), "r" (a6)
                    : "d0", "a0", "a1", "cc", "memory");
}

static void AacTraceInit(void)
{
    static const char dosName[] = "dos.library";
    if (!gAacTraceExec)
        gAacTraceExec = *((void **)4L);
    if (!gAacTraceDos)
        gAacTraceDos = AacTraceOpenLibrary(dosName, 0);
}
#else
static void AacTraceInit(void) { }
#endif

static void AacTraceAppendChar(char *buf, unsigned long *pos, unsigned long cap, char c)
{
    if (*pos + 1 < cap)
        buf[(*pos)++] = c;
}

static void AacTraceAppendStr(char *buf, unsigned long *pos, unsigned long cap, const char *s)
{
    while (s && *s)
        AacTraceAppendChar(buf, pos, cap, *s++);
}

static void AacTraceAppendLong(char *buf, unsigned long *pos, unsigned long cap, long v)
{
    char tmp[12];
    unsigned long n = 0;
    unsigned long u;
    if (v < 0) {
        AacTraceAppendChar(buf, pos, cap, '-');
        u = (unsigned long)(-v);
    } else {
        u = (unsigned long)v;
    }
    do {
        tmp[n++] = (char)('0' + (u % 10));
        u /= 10;
    } while (u && n < sizeof(tmp));
    while (n)
        AacTraceAppendChar(buf, pos, cap, tmp[--n]);
}

static void AacTraceAppendULong(char *buf, unsigned long *pos, unsigned long cap, unsigned long v)
{
    char tmp[12];
    unsigned long n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v && n < sizeof(tmp));
    while (n)
        AacTraceAppendChar(buf, pos, cap, tmp[--n]);
}

static void AacTraceAppendFieldUL(char *buf, unsigned long *pos, unsigned long cap,
                                  const char *name, unsigned long v)
{
    AacTraceAppendChar(buf, pos, cap, ' ');
    AacTraceAppendStr(buf, pos, cap, name);
    AacTraceAppendChar(buf, pos, cap, '=');
    AacTraceAppendULong(buf, pos, cap, v);
}

static void AacTraceAppendFieldL(char *buf, unsigned long *pos, unsigned long cap,
                                 const char *name, long v)
{
    AacTraceAppendChar(buf, pos, cap, ' ');
    AacTraceAppendStr(buf, pos, cap, name);
    AacTraceAppendChar(buf, pos, cap, '=');
    AacTraceAppendLong(buf, pos, cap, v);
}

static void AacTracePoint(AacState *st, const char *point, long err,
                          const AACFrameInfo *fi, long decodeReturn)
{
    (void)st;
    (void)point;
    (void)err;
    (void)fi;
    (void)decodeReturn;
}



static int AacValidateAdtsHeader(const unsigned char *p, int n)
{
    int profile, sfIndex, chanCfg, frameLen;

    if (!p || n < 7)
        return 0;
    if (p[0] != 0xff || (p[1] & 0xf0) != 0xf0)
        return 0;
    /* layer must be 0; protection_absent may be 0 or 1. */
    if ((p[1] & 0x06) != 0)
        return 0;

    profile = (p[2] >> 6) & 0x03;
    sfIndex = (p[2] >> 2) & 0x0f;
    chanCfg = ((p[2] & 0x01) << 2) | ((p[3] >> 6) & 0x03);
    frameLen = ((p[3] & 0x03) << 11) | (p[4] << 3) | ((p[5] >> 5) & 0x07);

    if (profile != 1)          /* AAC LC */
        return 0;
    if (sfIndex == 0x0f)       /* explicit frequency not valid in ADTS */
        return 0;
    if (chanCfg == 0 || chanCfg > 2)
        return 0;
    if (frameLen < 7 || frameLen > n)
        return 0;

    return 1;
}

static int AacFindSyncBounded(AacState *st)
{
    unsigned long searches = 0;
    int offset;

    for (;;) {
        offset = AACFindSyncWord(st->iobufReadPtr, st->iobufLeft);
        if (offset >= 0) {
            st->lastSyncOffset = (unsigned long)offset;
            return offset;
        }
        st->lastSyncOffset = (unsigned long)-1;
        if (st->eof || ++searches >= AAC_SYNC_SEARCH_GUARD)
            return -1;
        if (!AacRefillBuf(st))
            return -1;
    }
}


static void AacClearOutput(AacState *st)
{
    if (!st)
        return;
    st->outbufFill = 0;
    st->outbufPos = 0;
    st->lastSamplesProduced = 0;
}

static int AacResyncNextAdts(AacState *st)
{
    unsigned long searches = 0;

    AacClearOutput(st);

    if (!st || !st->iobuf || !st->iobufReadPtr)
        return 0;

    /* Do not retry the same damaged frame forever.  Move at least one byte
     * forward, then search/refill for the next plausible ADTS frame. */
    if (st->iobufLeft > 0) {
        st->iobufReadPtr++;
        st->iobufLeft--;
    }

    for (;;) {
        int offset;

        if ((unsigned long)st->iobufLeft < AAC_REFILL_THRESH && !st->eof)
            AacRefillBuf(st);

        if (st->iobufLeft <= 0) {
            st->done = 1;
            return 0;
        }

        offset = AACFindSyncWord(st->iobufReadPtr, st->iobufLeft);
        if (offset >= 0) {
            st->iobufReadPtr += offset;
            st->iobufLeft -= offset;
            st->lastSyncOffset = (unsigned long)offset;

            if ((unsigned long)st->iobufLeft < AAC_REFILL_THRESH && !st->eof)
                AacRefillBuf(st);

            if (AacValidateAdtsHeader(st->iobufReadPtr, st->iobufLeft)) {
                st->totalResyncs++;
                AacTracePoint(st, "11 resync next ADTS", st->lastDecodeErr, 0, 0);
                return 1;
            }

            st->totalInvalidFrames++;
            if (st->iobufLeft > 0) {
                st->iobufReadPtr++;
                st->iobufLeft--;
            }
            continue;
        }

        st->lastSyncOffset = (unsigned long)-1;
        if (st->eof || ++searches >= AAC_SYNC_SEARCH_GUARD)
            return 0;
        if (!AacRefillBuf(st))
            return 0;
    }
}

static void AacFreeState(AacState *st)
{
    if (!st) return;
    if (st->aacHandle) AACFreeDecoder(st->aacHandle);
    if (st->outbufAlloc) ModuleFree(st->outbufAlloc);
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
    unsigned char *oldInputPtr;
    int oldBytesLeft;
    unsigned long oldInputOff;

    AacTracePoint(st, "06 decode op entry", st ? st->lastDecodeErr : 0, 0, 0);

    if (st->error)
        return -1;

    if ((unsigned long)st->iobufLeft < AAC_REFILL_THRESH)
        AacRefillBuf(st);

    if (st->iobufLeft <= 0) {
        st->done = 1;
        return 0;
    }

    offset = AacFindSyncBounded(st);
    if (offset < 0) {
        AacClearOutput(st);
        st->totalInvalidFrames++;
        return -1;
    }
    st->iobufReadPtr += offset;
    st->iobufLeft    -= offset;

    if (!AacValidateAdtsHeader(st->iobufReadPtr, st->iobufLeft)) {
        AacClearOutput(st);
        st->totalInvalidFrames++;
        AacResyncNextAdts(st);
        return -1;
    }

    oldInputPtr = st->iobufReadPtr;
    oldBytesLeft = st->iobufLeft;
    oldInputOff = (unsigned long)(oldInputPtr - st->iobuf);
    st->lastBytesBefore = (unsigned long)oldBytesLeft;
    st->lastInputBefore = oldInputOff;
    st->lastSamplesProduced = 0;

    if (!AacCheckOutputCanary(st, "before normal AACDecode canary"))
        return -1;
    AacTracePoint(st, "07 before normal AACDecode", st->lastDecodeErr, 0, 0);
    err = AACDecode(st->aacHandle, &st->iobufReadPtr, &st->iobufLeft, st->outbuf);
    if (!AacCheckOutputCanary(st, "after normal AACDecode canary"))
        return -1;
    st->lastDecodeErr = err;
    st->lastBytesAfter = (unsigned long)(st->iobufLeft < 0 ? 0 : st->iobufLeft);
    st->lastInputAfter = (unsigned long)(st->iobufReadPtr - st->iobuf);
    AACGetLastFrameInfo(st->aacHandle, &fi);
    AacTracePoint(st, "08 after normal AACDecode returns", err, &fi, 0);
    if (err != 0) {
        st->totalDecodeErrors++;
        AacClearOutput(st);
        AacResyncNextAdts(st);
        return -1;
    }

    AacTracePoint(st, "09 after AACGetLastFrameInfo normal", err, &fi, 0);
    if (fi.outputSamps <= 0 || fi.nChans != st->channels ||
        (unsigned long)fi.outputSamps > AAC_OUT_CAP) {
        st->totalInvalidFrames++;
        AacClearOutput(st);
        AacResyncNextAdts(st);
        return -1;
    }

    /* Helix AAC reports outputSamps as total interleaved PCM shorts across
     * all channels (stereo AAC-LC normally reports 2048 total samples:
     * 1024 sample frames * 2 channels). */
    st->lastSamplesProduced = (unsigned long)fi.outputSamps;
    if (st->iobufReadPtr == oldInputPtr && st->iobufLeft == oldBytesLeft &&
        fi.outputSamps <= 0) {
        st->totalInvalidFrames++;
        AacClearOutput(st);
        AacResyncNextAdts(st);
        return -1;
    }

    /* Store total interleaved shorts; decode() converts to ABI sample frames. */
    st->outbufFill = (unsigned long)fi.outputSamps;
    st->outbufPos  = 0;
    AacTracePoint(st, "10 decode return value", err, &fi, 1);
    return 1;
}

static DecHandle AacOpen(DecoderReadCb readFn, DecoderSeekCb seekFn,
                         void *userData, struct DecoderStreamInfo *infoOut)
{
    AacState    *st;
    AACFrameInfo fi;
    int          offset, err;

    AacTraceInit();
    AacTracePoint(0, "01 AacOpen entry", 0, 0, 0);

    if (!readFn || !infoOut) return NULL;

#ifdef HAVE_AMIGA_AUDIO_DEVICE
    AacModuleSetExecBase(*((void **)4L));
#endif

    st = (AacState *)ModuleAlloc(sizeof(AacState));
    if (!st) return NULL;

    st->iobuf = (unsigned char *)ModuleAlloc(AAC_IO_CAP);
    if (!st->iobuf) { AacFreeState(st); return NULL; }
    st->iobufCap     = AAC_IO_CAP;
    st->iobufReadPtr = st->iobuf;
    st->iobufLeft    = 0;

    st->outbufAllocBytes = AAC_GUARD_BYTES + (AAC_OUT_CAP * (unsigned long)sizeof(short)) + AAC_GUARD_BYTES;
    st->outbufAlloc = (unsigned char *)ModuleAlloc(st->outbufAllocBytes);
    if (!st->outbufAlloc) { AacFreeState(st); return NULL; }
    st->outbuf = (short *)(st->outbufAlloc + AAC_GUARD_BYTES);
    AacInitOutputCanary(st);

    st->readFn   = readFn;
    st->seekFn   = seekFn;
    st->userData = userData;

    /* AACInitDecoder() allocates its internal state via malloc()
     * which is remapped to AacModuleMalloc through the -D flags. */
    st->aacHandle = AACInitDecoder();
    AacTracePoint(st, "02 after AACInitDecoder", st->aacHandle ? 0 : -1, 0, 0);
    if (!st->aacHandle) { AacFreeState(st); return NULL; }

    /* Prime buffer and find first ADTS sync word.  Do not feed MP4/M4A
     * container data or empty/unsynchronised input into AACDecode(). */
    if (!AacRefillBuf(st) || !st->iobufReadPtr || st->iobufLeft < 2) {
        AacFreeState(st); return NULL;
    }
    if (st->iobufReadPtr[0] != 0xff ||
        (st->iobufReadPtr[1] != 0xf1 && st->iobufReadPtr[1] != 0xf9)) {
        AacFreeState(st); return NULL;
    }

    offset = AacFindSyncBounded(st);
    if (offset < 0 || offset >= st->iobufLeft) { AacFreeState(st); return NULL; }
    st->iobufReadPtr += offset;
    st->iobufLeft    -= offset;

    if (!AacValidateAdtsHeader(st->iobufReadPtr, st->iobufLeft)) {
        AacFreeState(st); return NULL;
    }

    st->channels = ((st->iobufReadPtr[2] & 0x01) << 2) |
                   ((st->iobufReadPtr[3] >> 6) & 0x03);

    /* Decode first frame — probes sample rate, channel count, bit depth */
    {
        unsigned char *oldInputPtr = st->iobufReadPtr;
        int oldBytesLeft = st->iobufLeft;
        st->lastBytesBefore = (unsigned long)oldBytesLeft;
        st->lastInputBefore = (unsigned long)(oldInputPtr - st->iobuf);
        if (!AacCheckOutputCanary(st, "before first/probe AACDecode canary")) {
            AacFreeState(st); return NULL;
        }
        AacTracePoint(st, "03 before first/probe AACDecode", st->lastDecodeErr, 0, 0);
        err = AACDecode(st->aacHandle,
                        &st->iobufReadPtr,
                        &st->iobufLeft,
                        st->outbuf);
        if (!AacCheckOutputCanary(st, "after first/probe AACDecode canary")) {
            AacFreeState(st); return NULL;
        }
        st->lastDecodeErr = err;
        st->lastBytesAfter = (unsigned long)(st->iobufLeft < 0 ? 0 : st->iobufLeft);
        st->lastInputAfter = (unsigned long)(st->iobufReadPtr - st->iobuf);
        AACGetLastFrameInfo(st->aacHandle, &fi);
        AacTracePoint(st, "04 after first/probe AACDecode returns", err, &fi, 0);
        if (err != 0) {
            AacClearOutput(st);
            st->totalDecodeErrors++;
            AacFreeState(st); return NULL;
        }
    }

    AacTracePoint(st, "05 after AACGetLastFrameInfo probe", st->lastDecodeErr, &fi, 0);
    if (st->iobufReadPtr == st->iobuf + st->lastInputBefore &&
        st->iobufLeft == (int)st->lastBytesBefore && fi.outputSamps <= 0) {
        AacFreeState(st); return NULL;
    }
    if (fi.nChans <= 0 || fi.sampRateOut <= 0 ||
        fi.nChans > 2  || fi.outputSamps <= 0 ||
        (unsigned long)fi.outputSamps > AAC_OUT_CAP) {
        AacFreeState(st); return NULL;
    }

    st->channels   = fi.nChans;
    /* fi.outputSamps is total interleaved shorts, not samples/channel. */
    st->outbufFill = (unsigned long)fi.outputSamps;
    st->outbufPos  = 0;
    st->lastSamplesProduced = (unsigned long)fi.outputSamps;

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
    DecULong     produced = 0; /* ABI return value: sample frames per channel */
    unsigned long ch;

    if (!st || !outBuf || maxSamplesPerChan == 0) return -1;
    if (st->error) return -1;
    if (st->done)  return 0;

    ch = (unsigned long)st->channels;

    while (produced < maxSamplesPerChan) {
        /* Drain PCM left from the previous decode call */
        if (st->outbufPos < st->outbufFill) {
            unsigned long avail = (st->outbufFill - st->outbufPos) / ch;
            unsigned long take = (unsigned long)(maxSamplesPerChan - produced);
            unsigned long i, total;
            if (!AacCheckOutputCanary(st, "before AAC output copy canary"))
                return -1;
            if (take > avail) take = avail;
            total = take * ch;
            {
                const short *src = st->outbuf + st->outbufPos;
                short *dst = outBuf + produced * ch;
                for (i = 0; i < total; i++)
                    dst[i] = src[i];
            }
            if (!AacCheckOutputCanary(st, "after AAC output copy canary"))
                return -1;
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
            st->errCount++;
            AacTracePoint(st, "12 recoverable decode skip", st->lastDecodeErr, 0, -1);
            if (st->errCount > AAC_MAX_ERRORS) {
                st->error = 1;
                return -1;
            }
            continue;
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

__attribute__((used))
struct DecoderOps gAacOps = {
    &gAacInfo,
    AacOpen,
    AacDecode,
    AacSeek,
    AacClose,
    NULL
};
