/*
 * flac_module.c - FLAC decoder module for MiniAMP3 using libfoxenflac.
 *
 * The module implements the DecoderOps vtable declared in decoder_module.h.
 * All memory is allocated via exec AllocMem (MEMF_FAST) on Amiga.
 *
 * Sized for Subset-format FLAC up to 48 kHz, stereo (uses ~50 KB Fast RAM).
 */

#include "decoder_module.h"
#include "flac_alloc.h"
#include "flac/src/foxen-flac.h"


#ifndef FLAC_MODULE_DEBUG
#define FLAC_MODULE_DEBUG 0
#endif

#if FLAC_MODULE_DEBUG
/* Decoder-module logging must be supplied by a freestanding implementation. */
extern void FlacModuleDebug(const char *fmt, ...);
#define FLAC_DEBUG(...) FlacModuleDebug(__VA_ARGS__)
#else
#define FLAC_DEBUG(...) ((void)0)
#endif

static void *ModuleAlloc(unsigned long bytes)
{
    return FlacModuleCalloc(1, (size_t)bytes);
}

static void ModuleFree(void *ptr, unsigned long bytes)
{
    (void)bytes;
    FlacModuleFree(ptr);
}

/* Maximum block size and channels we support (Subset DAT: 4608 samples, 2 ch).
 * Files outside this range will fail to open. */
#define FLAC_BLK   FLAC_SUBSET_MAX_BLOCK_SIZE_48KHZ   /* 4608 */
#define FLAC_CH    2U

/* Compressed I/O buffer: read this many bytes from file per refill */
#define FLAC_IO_CAP  (16UL * 1024UL)

/* Output sample buffer capacity: full block * channels (interleaved int32_t) */
#define FLAC_OUT_CAP ((unsigned long)(FLAC_BLK) * (unsigned long)(FLAC_CH))
#define FLAC_STALL_LIMIT 64
#define FLAC_MODULE_BUILD_ID "FLAC MODULE BUILD MARKER 12345 rev 2"

static const char *FlacStateName(fx_flac_state_t state)
{
    switch (state) {
    case FLAC_INIT: return "FLAC_INIT";
    case FLAC_END_OF_METADATA: return "FLAC_END_OF_METADATA";
    case FLAC_DECODED_FRAME: return "FLAC_DECODED_FRAME";
    case FLAC_ERR: return "FLAC_ERR";
    default: return "FLAC_STATE_UNKNOWN";
    }
}

typedef struct FlacState {
    void         *flacMem;   /* original AllocMem result for flac state */
    fx_flac_t    *flac;      /* pointer returned by fx_flac_init (may differ) */
    unsigned long flacSize;

    unsigned char *iobuf;
    unsigned long  iobufFill; /* valid bytes from start of iobuf */
    unsigned long  iobufPos;  /* read cursor into iobuf */

    int32_t       *outbuf;    /* decoded interleaved int32_t samples */
    unsigned long  outbufFill;/* valid int32_t samples in outbuf */
    unsigned long  outbufPos; /* read cursor (in int32_t samples) */

    int  channels;
    int  shift;     /* right-shift to reduce to 16-bit range */

    DecoderReadCb  readFn;
    DecoderSeekCb  seekFn;
    void          *userData;

    int  eof;
    int  error;
    int  done;
    unsigned long stallCount;

    /* Host-visible in debugger/core dumps without module-side logging. */
    unsigned long lastFlacState;
    unsigned long lastBytesFed;
    unsigned long lastBytesConsumed;
    unsigned long lastSamplesProduced;
} FlacState;
static void FlacFreeState(FlacState *st)
{
    if (!st) return;
    if (st->outbuf)  ModuleFree(st->outbuf,  FLAC_OUT_CAP * sizeof(int32_t));
    if (st->iobuf)   ModuleFree(st->iobuf,   FLAC_IO_CAP);
    if (st->flacMem) ModuleFree(st->flacMem, st->flacSize);
    ModuleFree(st, sizeof(FlacState));
}


/* Refill iobuf from file if the current buffer is exhausted. */
static void FlacDumpFirstBytes(const unsigned char *buf, unsigned long count)
{
    unsigned long i, n = (count < 16UL) ? count : 16UL;
    FLAC_DEBUG("flac-debug: read first16");
    for (i = 0; i < n; i++)
        FLAC_DEBUG(" %02lx", (unsigned long)buf[i]);
    FLAC_DEBUG("\n");
}

/* Refill iobuf from file if the current buffer is exhausted. */
static int FlacFillBuf(FlacState *st)
{
    DecLong got;
    if (st->eof)
        return (st->iobufPos < st->iobufFill);
    if (st->iobufPos >= st->iobufFill) {
        FLAC_DEBUG("flac-debug: read request size=%lu\n", (unsigned long)FLAC_IO_CAP);
        got = st->readFn(st->userData, st->iobuf, FLAC_IO_CAP);
        FLAC_DEBUG("flac-debug: read returned size=%ld\n", (long)got);
        if (got <= 0) {
            st->eof = 1;
            FLAC_DEBUG("flac-debug: read eof_set=%d\n", st->eof);
            return 0;
        }
        st->iobufFill = (unsigned long)got;
        st->iobufPos  = 0;
        FLAC_DEBUG("flac-debug: read eof_set=%d\n", st->eof);
        FlacDumpFirstBytes(st->iobuf, st->iobufFill);
    }
    return 1;
}

/*
 * Run fx_flac_process until we have a decoded audio block in outbuf,
 * EOF, or error.  Returns 1 with outbuf populated, 0 for EOF, -1 for error.
 */
static int FlacRunDecoder(FlacState *st)
{
    unsigned long guard = 0;

    if (st->error)
        return -1;
    if (st->done)
        return 0;

    for (;;) {
        uint32_t in_avail;
        uint32_t out_avail;
        fx_flac_state_t state;
        uint32_t fed;
        uint32_t consumed;
        uint32_t produced;

        if (++guard > FLAC_STALL_LIMIT) {
            st->error = 1;
            return -1;
        }

        FLAC_DEBUG("flac-debug: FlacRunDecoder before FlacFillBuf iobufFill=%lu iobufPos=%lu\n",
               st->iobufFill, st->iobufPos);
        if (!FlacFillBuf(st)) {
            st->done = 1;
            FLAC_DEBUG("flac-debug: FlacRunDecoder FlacFillBuf result=0 iobufFill=%lu iobufPos=%lu done=%d eof=%d error=%d\n",
                   st->iobufFill, st->iobufPos, st->done, st->eof, st->error);
            return 0;
        }
        FLAC_DEBUG("flac-debug: FlacRunDecoder FlacFillBuf result=1 iobufFill=%lu iobufPos=%lu\n",
               st->iobufFill, st->iobufPos);

        in_avail  = (uint32_t)(st->iobufFill - st->iobufPos);
        out_avail = (uint32_t)FLAC_OUT_CAP;
        fed = in_avail;
        FLAC_DEBUG("flac-debug: fx_flac_process before bytes_available=%lu out_avail=%lu\n",
               (unsigned long)in_avail, (unsigned long)out_avail);

        state = fx_flac_process(st->flac,
                                st->iobuf + st->iobufPos,
                                &in_avail,
                                st->outbuf,
                                &out_avail);

        /* On return libfoxenflac writes bytes consumed to in_avail and
         * individual int32_t samples produced to out_avail. */
        consumed = in_avail;
        produced = out_avail;
        st->lastFlacState = (unsigned long)state;
        st->lastBytesFed = (unsigned long)fed;
        st->lastBytesConsumed = (unsigned long)consumed;
        st->lastSamplesProduced = (unsigned long)produced;
        FLAC_DEBUG("flac-debug: fx_flac_process returned state=%s consumed=%lu produced=%lu\n",
               FlacStateName(state), (unsigned long)consumed, (unsigned long)produced);

        if (consumed > fed) {
            st->error = 1;
            return -1;
        }
        st->iobufPos += (unsigned long)consumed;
        FLAC_DEBUG("flac-debug: FlacRunDecoder after consume iobufFill=%lu iobufPos=%lu\n",
               st->iobufFill, st->iobufPos);

        if (consumed == 0 && produced == 0 && state != FLAC_ERR) {
            st->stallCount++;
            if (st->stallCount > FLAC_STALL_LIMIT) {
                FLAC_DEBUG("flac-debug: decoder stalled state=%s fed=%lu eof=%d error=%d\n",
                       FlacStateName(state), (unsigned long)fed, st->eof, st->error);
                st->error = 1;
                return -1;
            }
        } else {
            st->stallCount = 0;
        }

        if (state == FLAC_ERR) {
            st->error = 1;
            return -1;
        }

        if (produced > 0) {
            if (st->channels <= 0 || (produced % (uint32_t)st->channels) != 0) {
                st->error = 1;
                return -1;
            }
            st->outbufFill = (unsigned long)produced;
            st->outbufPos  = 0;
            return 1;
        }

        /* Consuming metadata, frame headers, or other non-frame states without
         * PCM is valid.  Keep driving the decoder instead of reporting EOF to
         * the host.  Repeated no-consume/no-output iterations are caught by
         * stallCount/guard above. */
    }
}

static DecHandle FlacOpen(DecoderReadCb readFn, DecoderSeekCb seekFn,
                          void *userData, struct DecoderStreamInfo *infoOut)
{
    FlacState    *st;
    unsigned long flacSize;
    int64_t       sr, nch, ss, ns;
    fx_flac_state_t state;
    int tries;

    flacSize = (unsigned long)fx_flac_size(FLAC_BLK, FLAC_CH);
    if (flacSize == 0)
        return NULL;

    st = (FlacState *)ModuleAlloc(sizeof(FlacState));
    if (!st) return NULL;

    st->flacMem  = ModuleAlloc(flacSize);
    st->flacSize = flacSize;
    if (!st->flacMem) {
        FlacFreeState(st);
        return NULL;
    }

    st->flac = fx_flac_init(st->flacMem, (uint16_t)FLAC_BLK, (uint8_t)FLAC_CH);
    if (!st->flac) {
        FlacFreeState(st);
        return NULL;
    }

    st->iobuf = (unsigned char *)ModuleAlloc(FLAC_IO_CAP);
    if (!st->iobuf) {
        FlacFreeState(st);
        return NULL;
    }

    st->outbuf = (int32_t *)ModuleAlloc(FLAC_OUT_CAP * sizeof(int32_t));
    if (!st->outbuf) {
        FlacFreeState(st);
        return NULL;
    }

    st->readFn   = readFn;
    st->seekFn   = seekFn;
    st->userData = userData;

    /* Feed until FLAC_END_OF_METADATA so streaminfo is available */
    state = FLAC_INIT;
    tries = 0;
    while (state != FLAC_END_OF_METADATA && state != FLAC_ERR && tries < 2048) {
        uint32_t in_avail, out_avail;
        uint32_t fed, consumed, produced;
        if (!FlacFillBuf(st))
            break;
        if (st->iobufPos >= st->iobufFill)
            break;
        in_avail  = (uint32_t)(st->iobufFill - st->iobufPos);
        out_avail = 0;
        fed = in_avail;
        FLAC_DEBUG("flac-debug: open metadata before fx_flac_process bytes_available=%lu iobufFill=%lu iobufPos=%lu\n",
               (unsigned long)in_avail, st->iobufFill, st->iobufPos);
        state = fx_flac_process(st->flac, st->iobuf + st->iobufPos,
                                &in_avail, NULL, &out_avail);
        consumed = in_avail;
        produced = out_avail;
        st->lastFlacState = (unsigned long)state;
        st->lastBytesFed = (unsigned long)fed;
        st->lastBytesConsumed = (unsigned long)consumed;
        st->lastSamplesProduced = (unsigned long)produced;
        if (consumed > fed) {
            state = FLAC_ERR;
            break;
        }
        st->iobufPos += (unsigned long)consumed;
        if (consumed == 0 && produced == 0 && state != FLAC_ERR) {
            st->stallCount++;
            if (st->stallCount > FLAC_STALL_LIMIT) {
                state = FLAC_ERR;
                break;
            }
        } else {
            st->stallCount = 0;
        }
        FLAC_DEBUG("flac-debug: open metadata after fx_flac_process state=%s consumed=%lu produced=%lu iobufFill=%lu iobufPos=%lu\n",
               FlacStateName(state), (unsigned long)consumed,
               (unsigned long)produced, st->iobufFill, st->iobufPos);
        tries++;
    }

    st->stallCount = 0;

    if (state != FLAC_END_OF_METADATA) {
        FLAC_DEBUG("flac-debug: open result=failure streaminfo_parse_state=%s tries=%d eof=%d\n",
               FlacStateName(state), tries, st->eof);
        FlacFreeState(st);
        return NULL;
    }

    sr  = fx_flac_get_streaminfo(st->flac, FLAC_KEY_SAMPLE_RATE);
    nch = fx_flac_get_streaminfo(st->flac, FLAC_KEY_N_CHANNELS);
    ss  = fx_flac_get_streaminfo(st->flac, FLAC_KEY_SAMPLE_SIZE);
    ns  = fx_flac_get_streaminfo(st->flac, FLAC_KEY_N_SAMPLES);

    FLAC_DEBUG("flac-debug: open streaminfo sample_rate=%ld n_channels=%ld sample_size=%ld bits_per_sample=%ld n_samples=%ld\n",
           (long)sr, (long)nch, (long)ss, (long)ss, (long)ns);

    if (sr <= 0 || nch <= 0 || nch > (int64_t)FLAC_CH || ss <= 0) {
        FLAC_DEBUG("flac-debug: open result=failure invalid_streaminfo sample_rate=%ld n_channels=%ld sample_size=%ld n_samples=%ld\n",
               (long)sr, (long)nch, (long)ss, (long)ns);
        FlacFreeState(st);
        return NULL;
    }

    st->channels = (int)nch;
    /* fx_flac_process() returns signed PCM sample values in int32_t slots.
     * Preserve native 16-bit streams instead of shifting them down again; for
     * wider streams, discard only the extra low-order bits needed to produce
     * signed 16-bit PCM for the host DecoderOps ABI. */
    st->shift    = (ss > 16) ? (int)(ss - 16) : 0;

    infoOut->sampleRate    = (DecULong)sr;
    infoOut->channels      = (unsigned short)st->channels;
    infoOut->bitsPerSample = (unsigned short)(ss > 0 ? ss : 16);
    infoOut->totalSamples  = (ns > 0) ? (DecULong)ns : 0;

    FLAC_DEBUG("flac-debug: open result=success sample_rate=%ld n_channels=%ld sample_size=%ld n_samples=%ld iobufFill=%lu iobufPos=%lu\n",
           (long)sr, (long)nch, (long)ss, (long)ns, st->iobufFill, st->iobufPos);
    return (DecHandle)st;
}

static DecLong FlacDecode(DecHandle handle, short *outBuf, DecULong maxSamplesPerChan)
{
    FlacState    *st = (FlacState *)handle;
    DecULong      produced = 0;
    int           ch;
    unsigned long i, take, total;
    int32_t      *src;
    short        *dst;

    FLAC_DEBUG("FLACDECODE MARKER 12345\n");

    if (!st)
        return -1;

    FLAC_DEBUG("flac-debug: FlacDecode enter maxSamplesPerChannel=%lu st->pcmFrames=%lu st->pcmPos=%lu st->done=%d st->eof=%d st->error=%d st->iobufFill=%lu st->iobufPos=%lu\n",
           (unsigned long)maxSamplesPerChan,
           (unsigned long)(st->channels ? (st->outbufFill / (unsigned long)st->channels) : 0UL),
           (unsigned long)(st->channels ? (st->outbufPos / (unsigned long)st->channels) : 0UL),
           st->done, st->eof, st->error, st->iobufFill, st->iobufPos);

    if (st->error)
        return -1;
    if (st->done)
        return 0;

    ch = st->channels;

    while (produced < maxSamplesPerChan) {
        /* Drain already-decoded samples */
        if (st->outbufPos < st->outbufFill) {
            unsigned long avail = (st->outbufFill - st->outbufPos) / (unsigned long)ch;
            take = (unsigned long)(maxSamplesPerChan - produced);
            if (take > avail) take = avail;

            src = st->outbuf + st->outbufPos;
            dst = outBuf + produced * (unsigned long)ch;
            total = take * (unsigned long)ch;
#if defined(AMIGA_M68K) && defined(__GNUC__)
            /*
             * Fast path for streams where the selected scale is exactly a
             * high-word extraction from the int32_t PCM slot.
             */
            if (__builtin_expect(st->shift == 16 && total > 0, 1)) {
                const int32_t *s = src;
                short *d = dst;
                unsigned long cnt = total;
                __asm__ volatile (
                    "subq.l  #1,%2\n"
                    "1:\n\t"
                    "move.w  (%1),(%0)+\n\t"
                    "addq.l  #4,%1\n\t"
                    "dbf     %2,1b\n"
                    : "+a"(d), "+a"(s), "+d"(cnt)
                    :
                    : "cc", "memory"
                );
            } else
#endif
            for (i = 0; i < total; i++)
                dst[i] = (short)(src[i] >> st->shift);

            st->outbufPos += take * (unsigned long)ch;
            produced      += (DecULong)take;
            continue;
        }

        /* Need a new decoded block */
        FLAC_DEBUG("flac-debug: FlacDecode calling FlacRunDecoder pcmFrames=%lu pcmPos=%lu done=%d eof=%d error=%d\n",
               (unsigned long)(st->outbufFill / (unsigned long)ch),
               (unsigned long)(st->outbufPos / (unsigned long)ch),
               st->done, st->eof, st->error);
        {
            int runRc = FlacRunDecoder(st);
            FLAC_DEBUG("flac-debug: FlacDecode FlacRunDecoder called=1 rc=%d pcmFrames=%lu pcmPos=%lu done=%d eof=%d error=%d\n",
                   runRc,
                   (unsigned long)(st->outbufFill / (unsigned long)ch),
                   (unsigned long)(st->outbufPos / (unsigned long)ch),
                   st->done, st->eof, st->error);
            if (runRc <= 0)
                break;
        }
    }

    if (st->error)
        return -1;
    if (produced == 0 && !st->eof && !st->done) {
        FLAC_DEBUG("flac-debug: decoder stalled produced=0 eof=%d done=%d error=%d\n",
               st->eof, st->done, st->error);
        st->error = 1;
        return -1;
    }
    return (DecLong)produced;
}

static DecLong FlacSeek(DecHandle handle, DecULong sampleOffset)
{
    (void)handle; (void)sampleOffset;
    return -1;
}

static void FlacClose(DecHandle handle)
{
    FlacState *st = (FlacState *)handle;
    if (!st) return;
    FlacFreeState(st);
}

static struct DecoderModuleInfo gFlacInfo = {
    DECODER_MODULE_MAGIC,
    DECODER_MODULE_VERSION,
    0,
    FLAC_MODULE_BUILD_ID,
    "flac\0fla\0",
    0
};

struct DecoderOps gFlacOps = {
    &gFlacInfo,
    FlacOpen,
    FlacDecode,
    FlacSeek,
    FlacClose,
    NULL
};
