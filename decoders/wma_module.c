/*
 * wma_module.c - WMA (v1/v2) decoder module for MiniAMP3.
 *
 * Bridges the DecoderOps vtable (decoder_module.h) onto:
 *   - wma_asf.c: this project's own ASF container demuxer (see wma_asf.h)
 *   - decoders/wma/wmadeci.c: Rockbox's fixed-point WMA v1/v2 decoder
 *     (lib/rbcodec/codecs/libwma/wmadeci.c), vendored verbatim plus a
 *     small adaptation shim -- see decoders/wma/platform.h and
 *     decoders/wma/codeclib.h for what changed and why.
 *
 * The decode loop below (open once -> read a packet -> superframe_init ->
 * superframe_frame per frame in the packet -> repeat) mirrors Rockbox's
 * own codec wrapper (lib/rbcodec/codecs/wma.c codec_run()) exactly, since
 * that ordering encodes real bit-reservoir/state requirements that aren't
 * obvious from the function names alone.
 */

#include "decoder_module.h"
#include "wma_alloc.h"
#include "wma_asf.h"
#include "wma/wmadec.h"

#define WMA_MODULE_BUILD_ID "WMA MODULE BUILD MARKER 12345 rev 1"
#define WMA_IO_HINT_BYTES (32UL * 1024UL)
#define WMA_MAX_ERRORS 5

typedef struct WmaState {
    WmaAsfDemux       *asf;
    WMADecodeContext  *wmadec; /* heap-allocated: too large for the stack */

    const unsigned char *audiobuf;
    unsigned long         audiobufsize;
    int                    frameIdx;
    int                    errorCount;

    /* Interleaved S16 staging buffer: wma_decode_superframe_frame() hands
     * back up to BLOCK_MAX_SIZE planar fixed32 samples per channel per
     * call; we convert the whole thing to interleaved S16 once and drain
     * it across possibly several decode() calls. */
    short         spill[MAX_CHANNELS * BLOCK_MAX_SIZE];
    unsigned long spillPos;   /* interleaved-sample cursor  */
    unsigned long spillCount; /* interleaved samples valid  */

    int channels;
    int eof;
    int error;
} WmaState;

/*
 * WMA's fixed32 PCM output isn't Q16.16 like most of the decoder's
 * internal arithmetic (PRECISION==16) -- Rockbox's own codec wrapper
 * (lib/rbcodec/codecs/wma.c codec_main()) calls
 * ci->configure(DSP_SET_SAMPLE_DEPTH, 29) before handing frame_out
 * straight to its pcmbuf, i.e. full scale is a 29-bit signed dynamic
 * range. Shifting by 13 (29-16) maps that down to 16-bit signed PCM.
 */
static short WmaSampleToS16(fixed32 v)
{
    long s = (long)v >> 13;
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    return (short)s;
}

static DecHandle WmaOpen(DecoderReadCb readFn, DecoderSeekCb seekFn,
                          void *userData, struct DecoderStreamInfo *infoOut)
{
    WmaState           *st;
    asf_waveformatex_t  wfx;

    st = (WmaState *)WmaModuleCalloc(1, sizeof(WmaState));
    if (!st)
        return NULL;

    st->asf = WmaAsfOpen(readFn, seekFn, userData, &wfx);
    if (!st->asf) {
        WmaModuleFree(st);
        return NULL;
    }

    st->wmadec = (WMADecodeContext *)WmaModuleCalloc(1, sizeof(WMADecodeContext));
    if (!st->wmadec) {
        WmaAsfClose(st->asf);
        WmaModuleFree(st);
        return NULL;
    }

    if (wma_decode_init(st->wmadec, &wfx) < 0) {
        WmaModuleFree(st->wmadec);
        WmaAsfClose(st->asf);
        WmaModuleFree(st);
        return NULL;
    }

    st->channels = (int)wfx.channels;
    st->frameIdx = 0;

    infoOut->sampleRate    = (DecULong)wfx.rate;
    infoOut->channels      = wfx.channels;
    infoOut->bitsPerSample = 16;
    infoOut->totalSamples  = 0; /* not derivable from the ASF header cheaply */

    return (DecHandle)st;
}

static DecLong WmaDecode(DecHandle handle, short *outBuf, DecULong maxSamplesPerChan)
{
    WmaState     *st = (WmaState *)handle;
    unsigned long produced = 0; /* samples per channel */
    int           ch;

    if (!st)
        return -1;
    if (st->error)
        return -1;
    if (maxSamplesPerChan == 0)
        return 0;

    ch = st->channels;

    while (produced < (unsigned long)maxSamplesPerChan) {
        unsigned long availSamples = st->spillCount - st->spillPos;
        unsigned long availFrames  = availSamples / (unsigned long)ch;

        if (availFrames > 0) {
            unsigned long want = (unsigned long)maxSamplesPerChan - produced;
            unsigned long take = (want < availFrames) ? want : availFrames;

            WmaMemcpy(outBuf + produced * (unsigned long)ch,
                      st->spill + st->spillPos,
                      take * (unsigned long)ch * sizeof(short));
            st->spillPos += take * (unsigned long)ch;
            produced     += take;
            continue;
        }

        if (st->frameIdx >= st->wmadec->nb_frames) {
            const unsigned char *buf;
            unsigned long         len;
            int                   rc = WmaAsfReadPacket(st->asf, &buf, &len);

            if (rc == 0) {
                st->eof = 1;
                break;
            }
            if (rc < 0) {
                st->error = 1;
                return -1;
            }

            st->audiobuf     = buf;
            st->audiobufsize = len;
            st->frameIdx     = 0;

            if (!wma_decode_superframe_init(st->wmadec, st->audiobuf,
                                             (int)st->audiobufsize))
                continue; /* buf_size==0: shouldn't happen (len>0 on rc==1) */
            if (st->wmadec->nb_frames <= 0)
                continue; /* nothing to decode in this packet, try the next */
        }

        {
            int wmares = wma_decode_superframe_frame(st->wmadec, st->audiobuf,
                                                       (int)st->audiobufsize);
            st->frameIdx++;

            if (wmares < 0) {
                /* Mirrors Rockbox's own codec wrapper: a bad frame doesn't
                 * kill the whole stream, but repeated failures do. */
                st->frameIdx = st->wmadec->nb_frames; /* force next packet */
                st->errorCount++;
                if (st->errorCount > WMA_MAX_ERRORS) {
                    st->error = 1;
                    return -1;
                }
                continue;
            }
            st->errorCount = 0;
            if (wmares == 0)
                continue;

            {
                unsigned long n = (unsigned long)wmares;
                unsigned long i;
                int           c;

                if (n > (unsigned long)BLOCK_MAX_SIZE)
                    n = (unsigned long)BLOCK_MAX_SIZE; /* defensive clamp */

                for (i = 0; i < n; i++)
                    for (c = 0; c < ch; c++)
                        st->spill[i * (unsigned long)ch + (unsigned long)c] =
                            WmaSampleToS16((*st->wmadec->frame_out)[c][i]);

                st->spillPos   = 0;
                st->spillCount = n * (unsigned long)ch;
            }
        }
    }

    if (produced == 0 && st->error)
        return -1;
    return (DecLong)produced;
}

static DecLong WmaSeek(DecHandle handle, DecULong samplePos)
{
    /* WMA's bit-reservoir superframes make random-access seeking
     * state-dependent, same limitation as this project's FLAC/AAC/OGG
     * modules. */
    (void)handle; (void)samplePos;
    return -1;
}

static DecLong WmaGetIoHints(DecHandle handle, struct DecoderIoHints *out)
{
    (void)handle;
    if (!out)
        return -1;
    out->preferred_read_bytes = WMA_IO_HINT_BYTES;
    out->prefetch_bytes       = WMA_IO_HINT_BYTES * 2UL;
    return 0;
}

static void WmaClose(DecHandle handle)
{
    WmaState *st = (WmaState *)handle;
    if (!st)
        return;
    if (st->wmadec)
        WmaModuleFree(st->wmadec);
    if (st->asf)
        WmaAsfClose(st->asf);
    WmaModuleFree(st);
}

static struct DecoderModuleInfo gWmaInfo = {
    DECODER_MODULE_MAGIC,
    DECODER_MODULE_VERSION,
    0,
    WMA_MODULE_BUILD_ID,
    "wma\0",
    0
};

struct DecoderOps gWmaOps = {
    &gWmaInfo,
    WmaOpen,
    WmaDecode,
    WmaSeek,
    WmaClose,
    NULL,           /* get_tag */
    WmaGetIoHints
};
