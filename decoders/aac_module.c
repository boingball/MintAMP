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

#ifndef AAC_MODULE_DEBUG
#define AAC_MODULE_DEBUG 0
#endif

#if AAC_MODULE_DEBUG
extern void AacModuleDebug(const char *fmt, ...);
#define AAC_DEBUG(...) AacModuleDebug(__VA_ARGS__)
#else
#define AAC_DEBUG(...) ((void)0)
#endif

#define AAC_MODULE_BUILD_ID "AAC MODULE BUILD MARKER 12345 rev 3"

/* Compressed input ring buffer.  Must hold at least two maximum ADTS frames
 * (AAC_MAINBUF_SIZE = 768*2 = 1536 bytes each) to guarantee AACDecode always
 * has a full frame available.  8 KB amortises Exec I/O calls nicely. */
#define AAC_IO_CAP  (8UL * 1024UL)

/* PCM output: one decoded frame (1024 samples * 2 channels max).
 * SBR doubles to 2048 samples/ch, but SBR is disabled in this build. */
/* Leave margin for decoders that can report larger frames (for example SBR
 * builds producing 2048 samples/ch stereo).  Capacity is interleaved shorts. */
#define AAC_OUT_CAP  (2048UL * 2UL)

/* Refill the I/O buffer when fewer than this many bytes remain.
 * AAC_MAINBUF_SIZE guarantees at least one full frame fits after refill. */
#define AAC_REFILL_THRESH  ((unsigned long)AAC_MAINBUF_SIZE)

/* Give up on a stream after this many consecutive frame errors. */
#define AAC_MAX_ERRORS  64
#define AAC_SYNC_SEARCH_GUARD  4

/* White-noise / mis-decode burst detector thresholds (see AacPcmBurstCheck).
 * A garbage frame surfaces as broadband, noise-like PCM that is several times
 * louder than the surrounding music; these constants keep genuine loud passages
 * and sharp transients from being discarded. */
#define AAC_GLITCH_ABS_FLOOR   24000UL  /* loud burst: peak near full scale     */
#define AAC_GLITCH_MOD_FLOOR    6000UL  /* moderate burst: peak floor           */
#define AAC_GLITCH_REF_FLOOR    1024UL  /* need an established signal reference  */
#define AAC_GLITCH_RATIO           4UL  /* peak jump vs recent envelope         */
/* Spectral-flatness gate for moderate bursts, expressed as mean |x[n]-x[n-1]|
 * over mean |x[n]|, scaled by 256.  White noise sits near 340 (~1.33); tonal
 * and most percussive music stays well below 1.0, so 320 keeps real audio. */
#define AAC_GLITCH_HF_RATIO      320UL

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

    /* Debug/progress state visible in memory dumps without module-side printf. */
    unsigned long lastSyncOffset;
    unsigned long lastBytesBefore;
    unsigned long lastBytesAfter;
    unsigned long lastInputBefore;
    unsigned long lastInputAfter;
    unsigned long lastSamplesProduced;
    unsigned long lastFrameLen;
    int           lastDecodeErr;
    int           lastPeakMin;
    int           lastPeakMax;
    unsigned long lastFullScaleCount;
    unsigned long recentPeak;   /* decaying envelope of accepted-frame peaks */
} AacState;

static int AacRefillBuf(AacState *st);

static int AacParseAdtsHeader(const unsigned char *p, int n,
                               int *frameLenOut, int *channelsOut)
{
    int sfIndex, chanCfg, frameLen;

    if (frameLenOut) *frameLenOut = 0;
    if (channelsOut) *channelsOut = 0;

    if (!p || n < 7)
        return 0;
    if (p[0] != 0xff || (p[1] & 0xf0) != 0xf0)
        return 0;
    /* layer must be 0; protection_absent may be 0 or 1. */
    if ((p[1] & 0x06) != 0)
        return 0;

    sfIndex = (p[2] >> 2) & 0x0f;
    chanCfg = ((p[2] & 0x01) << 2) | ((p[3] >> 6) & 0x03);
    frameLen = ((p[3] & 0x03) << 11) | (p[4] << 3) | ((p[5] >> 5) & 0x07);

    if (sfIndex == 0x0f)       /* explicit frequency not valid in ADTS */
        return 0;
    if (chanCfg != 1 && chanCfg != 2)
        return 0;
    if (frameLen < ((p[1] & 0x01) ? 7 : 9))
        return 0;
    if (frameLen > AAC_MAINBUF_SIZE)
        return 0;
    if ((p[6] & 0x03) != 0)  /* one raw_data_block per ADTS frame */
        return 0;

    if (frameLenOut) *frameLenOut = frameLen;
    if (channelsOut) *channelsOut = chanCfg;
    return 1;
}

static int AacEnsureFullFrame(AacState *st, int frameLen)
{
    if (!st || frameLen < 7 || (unsigned long)frameLen > st->iobufCap)
        return 0;

    while (st->iobufLeft < frameLen) {
        if (st->eof)
            return 0;
        if (!AacRefillBuf(st) && st->iobufLeft < frameLen)
            return 0;
    }

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

static int AacFindNextValidAdtsOffset(AacState *st, int startOffset)
{
    unsigned char *base;
    int left, rel, frameLen, channels;

    if (!st || startOffset < 0 || startOffset >= st->iobufLeft)
        return -1;

    base = st->iobufReadPtr + startOffset;
    left = st->iobufLeft - startOffset;
    while (left >= 7) {
        rel = AACFindSyncWord(base, left);
        if (rel < 0)
            return -1;
        if (AacParseAdtsHeader(base + rel, left - rel, &frameLen, &channels) &&
            (unsigned long)frameLen <= st->iobufCap) {
            return startOffset + rel;
        }
        rel++;
        base += rel;
        left -= rel;
        startOffset += rel;
    }

    return -1;
}

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
 * Inspect a freshly decoded frame and reject it if it is a mis-decode burst.
 * Returns 1 to accept the PCM, 0 to drop the frame (the caller advances past
 * the consumed input and decodes the next frame, so a dropped frame becomes a
 * ~23 ms gap instead of an audible white-noise burst).
 */
static int AacPcmBurstCheck(AacState *st, int outputSamps)
{
    int i, sample, absSample, peakMin, peakMax, prev;
    unsigned long framePeak;
    unsigned long loudCount = 0;        /* samples above quarter scale */
    unsigned long fullScaleCount = 0;   /* samples pinned to the rails */
    unsigned long sumAbs = 0;           /* Sigma|x[n]|   (frame energy)        */
    unsigned long sumDiff = 0;          /* Sigma|x[n]-x[n-1]| (HF content)     */
    unsigned long hfRatio;
    unsigned long denom;
    unsigned long loudThreshold;
    unsigned long refPeak;

    if (!st || outputSamps <= 0)
        return 1;

    peakMin = 32767;
    peakMax = -32768;
    prev = 0;
    for (i = 0; i < outputSamps; i++) {
        sample = st->outbuf[i];
        if (sample < peakMin) peakMin = sample;
        if (sample > peakMax) peakMax = sample;
        absSample = sample < 0 ? -sample : sample;
        sumAbs += (unsigned long)absSample;
        if (i > 0) {
            int d = sample - prev;
            if (d < 0) d = -d;
            sumDiff += (unsigned long)d;
        }
        prev = sample;
        if (absSample > 8192) loudCount++;
        if (sample <= -32760 || sample >= 32760) fullScaleCount++;
    }

    /* Spectral-flatness proxy, scaled by 256.  Divide energy down first so the
     * intermediate products cannot overflow a 32-bit unsigned long. */
    denom = sumAbs >> 8;
    if (denom == 0) denom = 1;
    hfRatio = sumDiff / denom;

    framePeak = (unsigned long)(peakMax > 0 ? peakMax : 0);
    if (peakMin < 0 && (unsigned long)(-peakMin) > framePeak)
        framePeak = (unsigned long)(-peakMin);

    st->lastPeakMin = peakMin;
    st->lastPeakMax = peakMax;
    st->lastFullScaleCount = fullScaleCount;

    /* Pure rail-pinned noise: real music essentially never holds more than
     * half a frame at the exact clipping rails, so treat this as garbage
     * regardless of the surrounding level. */
    if (fullScaleCount > (unsigned long)outputSamps / 2) {
        AAC_DEBUG("aac-debug: dropped rail-pinned burst frameLen=%lu outputSamps=%ld fullScale=%lu peakMin=%ld peakMax=%ld\n",
                  st->lastFrameLen, (long)outputSamps, fullScaleCount,
                  (long)peakMin, (long)peakMax);
        return 0;
    }

    /*
     * Contextual glitch: a broadband frame, near full scale, that jumps several
     * times above the recent music level.  Require all three signals so that
     * genuine loud passages and sharp transients survive.  The reference is
     * built only from accepted frames, so a burst cannot raise the bar and mask
     * the next one; the reference floor keeps onsets after silence (where no
     * reference exists yet) from being discarded.
     */
    loudThreshold = (unsigned long)outputSamps / 4;
    refPeak = st->recentPeak;

    if (loudCount > loudThreshold &&
        framePeak >= AAC_GLITCH_ABS_FLOOR &&
        refPeak >= AAC_GLITCH_REF_FLOOR &&
        framePeak > refPeak * AAC_GLITCH_RATIO) {
        AAC_DEBUG("aac-debug: dropped loud burst frameLen=%lu outputSamps=%ld framePeak=%lu recentPeak=%lu loud=%lu/%lu fullScale=%lu\n",
                  st->lastFrameLen, (long)outputSamps, framePeak, refPeak,
                  loudCount, loudThreshold, fullScaleCount);
        return 0;
    }

    /*
     * Moderate glitch: a noise-like frame that jumps well above a quiet passage.
     * These are the residual white-noise clicks that are inaudible under loud
     * music but obvious in quiet sections.  The spectral-flatness gate keeps
     * tonal and percussive transients (which are not noise-like) intact.
     */
    if (framePeak >= AAC_GLITCH_MOD_FLOOR &&
        refPeak >= AAC_GLITCH_REF_FLOOR &&
        framePeak > refPeak * AAC_GLITCH_RATIO &&
        hfRatio >= AAC_GLITCH_HF_RATIO) {
        AAC_DEBUG("aac-debug: dropped noise burst frameLen=%lu outputSamps=%ld framePeak=%lu recentPeak=%lu hfRatio=%lu\n",
                  st->lastFrameLen, (long)outputSamps, framePeak, refPeak,
                  hfRatio);
        return 0;
    }

    /* Accepted: fold this frame's peak into the decaying envelope. */
    refPeak -= refPeak >> 3;            /* gentle decay between frames */
    if (framePeak > refPeak)
        refPeak = framePeak;
    st->recentPeak = refPeak;

    return 1;
}

/*
 * Decode exactly one AAC frame into st->outbuf.
 * Returns 1 with PCM ready, 0 for EOF, -1 for a recoverable frame error.
 */
static int AacDecodeFrame(AacState *st)
{
    int offset, err, frameLen, adtsChannels;
    unsigned char *decodePtr;
    int decodeLeft;
    AACFrameInfo fi;

    if (st->error)
        return -1;

    for (;;) {
        if ((unsigned long)st->iobufLeft < AAC_REFILL_THRESH)
            AacRefillBuf(st);

        if (st->iobufLeft <= 0) {
            st->done = 1;
            return 0;
        }

        offset = AacFindSyncBounded(st);
        if (offset < 0) {
            st->done = st->eof;
            return st->done ? 0 : -1;
        }
        st->iobufReadPtr += offset;
        st->iobufLeft    -= offset;

        if (!AacParseAdtsHeader(st->iobufReadPtr, st->iobufLeft,
                                &frameLen, &adtsChannels)) {
            st->iobufReadPtr++;
            st->iobufLeft--;
            st->errCount++;
            if (st->errCount >= AAC_MAX_ERRORS) {
                st->error = 1;
                return -1;
            }
            continue;
        }

        st->lastFrameLen = (unsigned long)frameLen;
        if (!AacEnsureFullFrame(st, frameLen)) {
            st->done = 1;
            return 0;
        }

        st->lastBytesBefore = (unsigned long)st->iobufLeft;
        st->lastInputBefore = (unsigned long)(st->iobufReadPtr - st->iobuf);
        st->lastSamplesProduced = 0;
        st->lastPeakMin = 0;
        st->lastPeakMax = 0;
        st->lastFullScaleCount = 0;

        decodePtr = st->iobufReadPtr;
        decodeLeft = frameLen;
        err = AACDecode(st->aacHandle, &decodePtr, &decodeLeft, st->outbuf);
        st->lastDecodeErr = err;
        st->lastBytesAfter = (unsigned long)(decodeLeft < 0 ? 0 : decodeLeft);
        st->lastInputAfter = (unsigned long)(decodePtr - st->iobuf);

        if (err != 0) {
            AAC_DEBUG("aac-debug: decode error err=%ld frameLen=%lu bytesBefore=%lu bytesAfter=%lu consecutiveErrors=%ld\n",
                      (long)err, st->lastFrameLen, st->lastBytesBefore,
                      st->lastBytesAfter, (long)(st->errCount + 1));
            st->iobufReadPtr = st->iobuf + st->lastInputBefore;
            st->iobufLeft = (int)st->lastBytesBefore;
            if (st->iobufLeft > 1) {
                int advance = AacFindNextValidAdtsOffset(st, 1);
                if (advance < 0) advance = 1;
                st->iobufReadPtr += advance;
                st->iobufLeft -= advance;
            }
            st->errCount++;
            if (st->errCount >= AAC_MAX_ERRORS) {
                st->error = 1;
                return -1;
            }
            continue;
        }

        AACGetLastFrameInfo(st->aacHandle, &fi);
        st->lastSamplesProduced = (unsigned long)(fi.outputSamps > 0 ? fi.outputSamps : 0);
        if (fi.outputSamps <= 0 || (unsigned long)fi.outputSamps > AAC_OUT_CAP ||
            (fi.nChans != 1 && fi.nChans != 2) ||
            fi.sampRateOut < 8000 || fi.sampRateOut > 96000 ||
            fi.nChans != adtsChannels) {
            AAC_DEBUG("aac-debug: invalid frame info frameLen=%lu outputSamps=%ld sampleRate=%ld channels=%ld consecutiveErrors=%ld\n",
                      st->lastFrameLen, (long)fi.outputSamps,
                      (long)fi.sampRateOut, (long)fi.nChans,
                      (long)(st->errCount + 1));
            if (st->iobufLeft >= frameLen) {
                st->iobufReadPtr += frameLen;
                st->iobufLeft -= frameLen;
            }
            st->errCount++;
            if (st->errCount >= AAC_MAX_ERRORS) {
                st->error = 1;
                return -1;
            }
            continue;
        }

        if (st->iobufLeft >= frameLen) {
            st->iobufReadPtr += frameLen;
            st->iobufLeft -= frameLen;
        } else {
            st->iobufReadPtr = st->iobuf + st->lastInputBefore;
            st->iobufLeft = (int)st->lastBytesBefore;
            st->error = 1;
            return -1;
        }

        if (!AacPcmBurstCheck(st, fi.outputSamps)) {
            st->errCount++;
            if (st->errCount >= AAC_MAX_ERRORS) {
                st->error = 1;
                return -1;
            }
            continue;
        }

        AAC_DEBUG("aac-debug: frameLen=%lu bytesBefore=%lu bytesAfter=%lu outputSamps=%ld sampleRate=%ld channels=%ld peakMin=%ld peakMax=%ld fullScale=%lu consecutiveErrors=%ld\n",
                  st->lastFrameLen, st->lastBytesBefore, st->lastBytesAfter,
                  (long)fi.outputSamps, (long)fi.sampRateOut, (long)fi.nChans,
                  (long)st->lastPeakMin, (long)st->lastPeakMax,
                  st->lastFullScaleCount, (long)st->errCount);

        st->channels = fi.nChans;
        st->outbufFill = (unsigned long)fi.outputSamps;
        st->outbufPos  = 0;
        return 1;
    }
}

static DecHandle AacOpen(DecoderReadCb readFn, DecoderSeekCb seekFn,
                         void *userData, struct DecoderStreamInfo *infoOut)
{
    AacState    *st;
    AACFrameInfo fi;
    int          offset, frameLen, adtsChannels;

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

    st->outbuf = (short *)ModuleAlloc(AAC_OUT_CAP * sizeof(short));
    if (!st->outbuf) { AacFreeState(st); return NULL; }

    st->readFn   = readFn;
    st->seekFn   = seekFn;
    st->userData = userData;

    /* AACInitDecoder() allocates its internal state via malloc()
     * which is remapped to AacModuleMalloc through the -D flags. */
    st->aacHandle = AACInitDecoder();
    if (!st->aacHandle) { AacFreeState(st); return NULL; }

    /* Prime buffer and find first ADTS sync word.  Do not feed MP4/M4A
     * container data or empty/unsynchronised input into AACDecode(). */
    if (!AacRefillBuf(st) || !st->iobufReadPtr || st->iobufLeft < 2) {
        AacFreeState(st); return NULL;
    }
    offset = AacFindSyncBounded(st);
    if (offset < 0 || offset >= st->iobufLeft) { AacFreeState(st); return NULL; }
    st->iobufReadPtr += offset;
    st->iobufLeft    -= offset;

    if (!AacParseAdtsHeader(st->iobufReadPtr, st->iobufLeft,
                            &frameLen, &adtsChannels) ||
        !AacEnsureFullFrame(st, frameLen)) {
        AacFreeState(st); return NULL;
    }

    st->channels = adtsChannels;
    st->lastFrameLen = (unsigned long)frameLen;

    /* Decode first frame — probes sample rate, channel count, bit depth. */
    if (AacDecodeFrame(st) <= 0) {
        AacFreeState(st); return NULL;
    }

    AACGetLastFrameInfo(st->aacHandle, &fi);
    if (fi.outputSamps <= 0 || (unsigned long)fi.outputSamps > AAC_OUT_CAP ||
        (fi.nChans != 1 && fi.nChans != 2) ||
        fi.sampRateOut < 8000 || fi.sampRateOut > 96000) {
        AacFreeState(st); return NULL;
    }

    st->channels = fi.nChans;

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
            st->error = 1;
            return -1;
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
