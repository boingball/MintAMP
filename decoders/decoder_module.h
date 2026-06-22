/*
 * decoder_module.h - ABI for pluggable audio decoder modules (Amiga hunk format)
 *
 * Each decoder is a separately compiled hunk binary loaded with LoadSeg().
 * The first callable LONG in the first code hunk is the module entry point,
 * which returns a pointer to a static DecoderOps vtable.
 *
 * ABI versioning: the vtable is append-only.  New fields go at the end only.
 * Callers check info->version <= DECODER_MODULE_VERSION before use.
 */

#ifndef DECODER_MODULE_H
#define DECODER_MODULE_H

#include <stddef.h>

/* --- Magic and version --------------------------------------------------- */

#define DECODER_MODULE_MAGIC   0x44454300UL  /* "DEC\0" */
#define DECODER_MODULE_VERSION 1

/* --- Flags ---------------------------------------------------------------- */

#define DECF_NEEDS_SEEK  (1UL << 0)  /* stream must support seeking */

/* --- Core types (plain C, no Amiga-specific headers needed in modules) --- */

typedef void  *DecHandle;
typedef long   DecLong;
typedef unsigned long DecULong;

/* --- Info structs -------------------------------------------------------- */

struct DecoderModuleInfo {
    DecULong      magic;        /* must equal DECODER_MODULE_MAGIC            */
    unsigned short version;     /* ABI version; reject if > supported         */
    unsigned short revision;    /* minor revision (informational only)         */
    const char   *name;         /* "FLAC", "WAV", "OGG", ...                  */
    const char   *extensions;   /* double-null list: "flac\0fla\0\0"          */
    DecULong      flags;        /* DECF_* bits                                */
};

struct DecoderStreamInfo {
    DecULong      sampleRate;
    unsigned short channels;    /* 1 or 2                                     */
    unsigned short bitsPerSample; /* source bit depth (informational)         */
    DecULong      totalSamples; /* samples per channel; 0 if unknown          */
};

/* --- I/O callbacks (host provides, module calls) ------------------------- */

/* Return bytes read; 0 = EOF; negative = error */
typedef DecLong (*DecoderReadCb)(void *userData, unsigned char *buf, DecULong maxBytes);

/* whence: 0=SEEK_SET 1=SEEK_CUR 2=SEEK_END  (same as fseek)
   Return 0 on success, non-zero if seek not supported or failed */
typedef DecLong (*DecoderSeekCb)(void *userData, DecLong offset, int whence);

/* --- vtable (append-only; do not reorder existing entries) -------------- */

struct DecoderOps {
    struct DecoderModuleInfo *info;

    /*
     * open() — probe the stream, allocate decoder state, fill *infoOut.
     * The module MUST use only MEMF_FAST (or portable malloc) for its state.
     * Returns an opaque handle on success, NULL on failure.
     */
    DecHandle (*open)(DecoderReadCb readFn, DecoderSeekCb seekFn,
                      void *userData, struct DecoderStreamInfo *infoOut);

    /*
     * decode() — produce up to maxSamplesPerChan samples per channel into
     * outBuf as interleaved signed 16-bit PCM (L0 R0 L1 R1 ... for stereo).
     * Returns samples-per-channel written; 0 = EOF; negative = error.
     * The module is responsible for its own internal I/O buffering.
     */
    DecLong (*decode)(DecHandle handle, short *outBuf, DecULong maxSamplesPerChan);

    /*
     * seek() — seek to an absolute sample position (per channel).
     * Returns 0 on success, non-zero if not supported or seek failed.
     */
    DecLong (*seek)(DecHandle handle, DecULong samplePos);

    /*
     * close() — free all resources allocated by open().
     */
    void (*close)(DecHandle handle);

    /*
     * get_tag() — optional metadata query; NULL if not implemented.
     * tagId is module-defined; valueOut receives a NUL-terminated string.
     * Returns bytes written (excl. NUL), or negative on error / not found.
     */
    DecLong (*get_tag)(DecHandle handle, DecULong tagId,
                       void *valueOut, DecULong maxBytes);
};

/*
 * Module entry point — must be the first callable address in the first code
 * hunk (i.e. at BADDR(LoadSeg(path)) + 4).
 *
 * Called with no arguments.  The module must set up SysBase (from address 4)
 * before any exec calls.  Returns a pointer to a static DecoderOps table,
 * or NULL if this CPU or configuration is not supported.
 */
typedef struct DecoderOps *(*DecoderModuleEntryFn)(void);

#endif /* DECODER_MODULE_H */
