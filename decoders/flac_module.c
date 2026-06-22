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
} FlacState;

/* Refill iobuf from file if the current buffer is exhausted. */
static int FlacFillBuf(FlacState *st)
{
    DecLong got;
    if (st->eof)
        return (st->iobufPos < st->iobufFill);
    if (st->iobufPos >= st->iobufFill) {
        got = st->readFn(st->userData, st->iobuf, FLAC_IO_CAP);
        if (got <= 0) { st->eof = 1; return 0; }
        st->iobufFill = (unsigned long)got;
        st->iobufPos  = 0;
    }
    return 1;
}

/*
 * Run fx_flac_process until we have a decoded audio block in outbuf,
 * or until EOF / error.  Returns 1 with outbuf populated, 0 otherwise.
 */
static int FlacRunDecoder(FlacState *st)
{
    if (st->error || st->done)
        return 0;

    for (;;) {
        uint32_t in_avail;
        uint32_t out_avail;
        fx_flac_state_t state;

        if (!FlacFillBuf(st)) {
            st->done = 1;
            return 0;
        }

        in_avail  = (uint32_t)(st->iobufFill - st->iobufPos);
        out_avail = (uint32_t)FLAC_OUT_CAP;

        state = fx_flac_process(st->flac,
                                st->iobuf + st->iobufPos,
                                &in_avail,
                                st->outbuf,
                                &out_avail);

        /* in_avail is now bytes consumed */
        st->iobufPos += (unsigned long)in_avail;

        if (state == FLAC_ERR) {
            st->error = 1;
            return 0;
        }

        if (state == FLAC_DECODED_FRAME && out_avail > 0) {
            st->outbufFill = (unsigned long)out_avail;
            st->outbufPos  = 0;
            return 1;
        }

        /* Other states (END_OF_METADATA, SEARCH_FRAME, IN_FRAME, END_OF_FRAME):
         * keep looping; refill input as needed. */
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
        ModuleFree(st, sizeof(FlacState));
        return NULL;
    }

    st->flac = fx_flac_init(st->flacMem, (uint16_t)FLAC_BLK, (uint8_t)FLAC_CH);
    if (!st->flac) {
        ModuleFree(st->flacMem, flacSize);
        ModuleFree(st, sizeof(FlacState));
        return NULL;
    }

    st->iobuf = (unsigned char *)ModuleAlloc(FLAC_IO_CAP);
    if (!st->iobuf) {
        ModuleFree(st->flacMem, flacSize);
        ModuleFree(st, sizeof(FlacState));
        return NULL;
    }

    st->outbuf = (int32_t *)ModuleAlloc(FLAC_OUT_CAP * sizeof(int32_t));
    if (!st->outbuf) {
        ModuleFree(st->iobuf,   FLAC_IO_CAP);
        ModuleFree(st->flacMem, flacSize);
        ModuleFree(st, sizeof(FlacState));
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
        FlacFillBuf(st);
        if (st->iobufPos >= st->iobufFill)
            break;
        in_avail  = (uint32_t)(st->iobufFill - st->iobufPos);
        out_avail = 0;
        state = fx_flac_process(st->flac, st->iobuf + st->iobufPos,
                                &in_avail, NULL, &out_avail);
        st->iobufPos += (unsigned long)in_avail;
        tries++;
    }

    if (state != FLAC_END_OF_METADATA) {
        ModuleFree(st->outbuf,  FLAC_OUT_CAP * sizeof(int32_t));
        ModuleFree(st->iobuf,   FLAC_IO_CAP);
        ModuleFree(st->flacMem, flacSize);
        ModuleFree(st, sizeof(FlacState));
        return NULL;
    }

    sr  = fx_flac_get_streaminfo(st->flac, FLAC_KEY_SAMPLE_RATE);
    nch = fx_flac_get_streaminfo(st->flac, FLAC_KEY_N_CHANNELS);
    ss  = fx_flac_get_streaminfo(st->flac, FLAC_KEY_SAMPLE_SIZE);
    ns  = fx_flac_get_streaminfo(st->flac, FLAC_KEY_N_SAMPLES);

    if (sr <= 0 || nch <= 0 || nch > (int64_t)FLAC_CH) {
        ModuleFree(st->outbuf,  FLAC_OUT_CAP * sizeof(int32_t));
        ModuleFree(st->iobuf,   FLAC_IO_CAP);
        ModuleFree(st->flacMem, flacSize);
        ModuleFree(st, sizeof(FlacState));
        return NULL;
    }

    st->channels = (int)nch;
    st->shift    = (ss > 16) ? (int)(ss - 16) : 0;

    infoOut->sampleRate    = (DecULong)sr;
    infoOut->channels      = (unsigned short)st->channels;
    infoOut->bitsPerSample = (unsigned short)(ss > 0 ? ss : 16);
    infoOut->totalSamples  = (ns > 0) ? (DecULong)ns : 0;

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

    if (!st || st->error || st->done)
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
            for (i = 0; i < total; i++)
                dst[i] = (short)(src[i] >> st->shift);

            st->outbufPos += take * (unsigned long)ch;
            produced      += (DecULong)take;
            continue;
        }

        /* Need a new decoded block */
        if (!FlacRunDecoder(st))
            break;
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
    ModuleFree(st->outbuf,  FLAC_OUT_CAP * sizeof(int32_t));
    ModuleFree(st->iobuf,   FLAC_IO_CAP);
    ModuleFree(st->flacMem, st->flacSize);
    ModuleFree(st, sizeof(FlacState));
}

static struct DecoderModuleInfo gFlacInfo = {
    DECODER_MODULE_MAGIC,
    DECODER_MODULE_VERSION,
    0,
    "FLAC",
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
