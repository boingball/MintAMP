/*
 * ogg_module.c - OGG/Vorbis decoder module for MintAMP using Tremor
 * (Xiph's fixed-point/integer-only Vorbis decoder) + libogg.
 *
 * Implements the DecoderOps vtable from decoder_module.h.  Follows the same
 * shape as flac_module.c and aac_module.c: sequential streaming decode only,
 * no seek() support, all memory via ogg_alloc's Exec-backed shim.
 *
 * SOURCE LAYOUT (verified against the checked-out
 * pschatzmann/arduino-libvorbis-tremor submodule):
 *   decoders/tremor/src/tremor/ivorbiscodec.h  - vorbis_info, core codec types
 *   decoders/tremor/src/tremor/ivorbisfile.h   - OggVorbis_File, ov_* API
 *   decoders/libogg/include/ogg/ogg.h          - pulled in transitively by
 *                                                 ivorbisfile.h via <ogg/ogg.h>
 * decoders/tremor/src/tremor/ also contains iseeking_example.c and
 * ivorbisfile_example.c (demo apps with their own main()) — these are
 * excluded from the module build in decoders/Makefile.
 *
 * We deliberately open the stream in Tremor's *non-seekable* streaming mode
 * (seek_func/close_func/tell_func all NULL in the ov_callbacks passed to
 * ov_open_callbacks).  That mirrors FLAC/AAC in this project, neither of
 * which support seek() either, and it sidesteps needing a working tell_func
 * — the decoder_module.h seek callback only reports success/failure, not
 * position, so it cannot back a correct byte-domain bisection seek anyway.
 */

#include "decoder_module.h"
#include "ogg_alloc.h"
#include "tremor/src/tremor/ivorbiscodec.h"
#include "tremor/src/tremor/ivorbisfile.h"
#include "../miniamp_memguard.h"
#include "../radio_stream.h"

#define OGG_MODULE_BUILD_ID "OGG MODULE BUILD MARKER 12345 rev 1"

/* Bail out of the decode loop after this many consecutive non-fatal
 * ov_read() holes (OV_HOLE and friends) instead of spinning forever. */
#define OGG_STALL_LIMIT 64

/* get_io_hints(): Vorbis bitstreams are typically lower bitrate than FLAC,
 * so a smaller read-ahead chunk is plenty. */
#define OGG_IO_HINT_BYTES (32UL * 1024UL)

typedef struct OggState {
    OggVorbis_File vf;
    int  vfOpen;   /* set once ov_open_callbacks() succeeds, so OggClose()
                     * knows whether ov_clear() is safe to call */

    DecoderReadCb  readFn;
    DecoderSeekCb  seekFn;
    void          *userData;

    int  channels;
    int  bitstream; /* current logical bitstream index (chained Ogg) */

    int  eof;
    int  error;
    int  done;
} OggState;

/*
 * ov_callbacks.read_func adapter: bridges stdio-style fread() semantics onto
 * the host's DecoderReadCb.  Called with size==1 by vorbisfile in practice,
 * but implemented generically.
 */
static size_t OggReadAdapter(void *ptr, size_t size, size_t nmemb, void *datasource)
{
    OggState *st = (OggState *)datasource;
    unsigned long want;
    DecLong got;

    if (size == 0 || nmemb == 0)
        return 0;

    want = (unsigned long)size * (unsigned long)nmemb;
    got = st->readFn(st->userData, (unsigned char *)ptr, want);
    if (got <= 0) {
        st->eof = (got == 0);
        return 0;
    }
    return (size_t)((unsigned long)got / (unsigned long)size);
}

static DecHandle OggOpen(DecoderReadCb readFn, DecoderSeekCb seekFn,
                         void *userData, struct DecoderStreamInfo *infoOut)
{
    OggState    *st;
    ov_callbacks cb;
    vorbis_info *vi;
    int          rc;

    /* Matches aac_module.c: set up the module allocator's ExecBase here,
     * before the first allocation, rather than in ogg_entry.c. */
    OggModuleSetExecBase(*((void **)4L));

    st = (OggState *)OggModuleCalloc(1, sizeof(OggState));
    if (!st)
        return NULL;

    st->readFn   = readFn;
    st->seekFn   = seekFn;
    st->userData = userData;

    cb.read_func  = OggReadAdapter;
    cb.seek_func  = NULL;
    cb.close_func = NULL;
    cb.tell_func  = NULL;

    rc = ov_open_callbacks((void *)st, &st->vf, NULL, 0, cb);
    if (rc < 0) {
        OggModuleFree(st);
        return NULL;
    }
    st->vfOpen = 1;

    vi = ov_info(&st->vf, -1);
    if (!vi || vi->channels <= 0 || vi->channels > 2 || vi->rate <= 0) {
        ov_clear(&st->vf);
        OggModuleFree(st);
        return NULL;
    }

    st->channels = vi->channels;

    infoOut->sampleRate    = (DecULong)vi->rate;
    infoOut->channels      = (unsigned short)vi->channels;
    infoOut->bitsPerSample = 16;
    infoOut->totalSamples  = 0; /* unknown: non-seekable streaming mode */

    return (DecHandle)st;
}

static DecLong OggDecode(DecHandle handle, short *outBuf, DecULong maxSamplesPerChan)
{
    OggState     *st = (OggState *)handle;
    unsigned long ch;
    unsigned long capBytes;
    unsigned long totalBytes;
    char         *dst;
    int           guard;

    if (!st)
        return -1;
    if (st->error)
        return -1;
    if (st->done)
        return 0;

    ch = (unsigned long)st->channels;
    if (ch == 0) {
        st->error = 1;
        return -1;
    }

    capBytes   = (unsigned long)maxSamplesPerChan * ch * (unsigned long)sizeof(short);
    dst        = (char *)outBuf;
    totalBytes = 0;
    guard      = 0;

    while (totalBytes < capBytes) {
        int  bitstream = st->bitstream;
        long wantBytes = (long)(capBytes - totalBytes);
        long gotBytes  = ov_read(&st->vf, dst + totalBytes, (int)wantBytes, &bitstream);

        if (gotBytes == 0) {
            /* Clean end of stream. */
            st->done = 1;
            break;
        }
        if (gotBytes < 0) {
            /* Recoverable hole (OV_HOLE et al.) — retry a bounded number of
             * times before giving up, matching FLAC_STALL_LIMIT's role. */
            if (++guard > OGG_STALL_LIMIT) {
                st->error = 1;
                return -1;
            }
            continue;
        }
        guard = 0;

        if (bitstream != st->bitstream) {
            /* Chained logical bitstream boundary: channel count / sample
             * rate may have changed.  Stop here so the host only ever sees
             * PCM that matches the DecoderStreamInfo reported at open(). */
            st->bitstream = bitstream;
            st->done = 1;
            break;
        }

        totalBytes += (unsigned long)gotBytes;
    }

    if (totalBytes == 0) {
        if (st->done)
            return 0;
        st->error = 1;
        return -1;
    }

    return (DecLong)(totalBytes / (ch * (unsigned long)sizeof(short)));
}

static DecLong OggSeek(DecHandle handle, DecULong samplePos)
{
    /* Opened in non-seekable streaming mode; matches FlacSeek/AacSeek. */
    (void)handle; (void)samplePos;
    return -1;
}

static DecLong OggGetIoHints(DecHandle handle, struct DecoderIoHints *out)
{
    (void)handle;
    if (!out) return -1;
    out->preferred_read_bytes = OGG_IO_HINT_BYTES;
    out->prefetch_bytes       = OGG_IO_HINT_BYTES * 2UL;
    return 0;
}

static void OggClose(DecHandle handle)
{
    OggState *st = (OggState *)handle;
    if (!st) return;
    Radio_CheckMiniMem("before OGG/Tremor cleanup");
    if (st->vfOpen)
        ov_clear(&st->vf);
    OggModuleFree(st);
    Radio_CheckMiniMem("after OGG/Tremor cleanup");
}

static struct DecoderModuleInfo gOggInfo = {
    DECODER_MODULE_MAGIC,
    DECODER_MODULE_VERSION,
    0,
    OGG_MODULE_BUILD_ID,
    "ogg\0oga\0",
    0
};

__attribute__((used))
struct DecoderOps gOggOps = {
    &gOggInfo,
    OggOpen,
    OggDecode,
    OggSeek,
    OggClose,
    NULL,           /* get_tag */
    OggGetIoHints
};
