#ifndef RADIO_OGGFLAC_H
#define RADIO_OGGFLAC_H

#include <stddef.h>
#include <string.h>

/* Lossless ("hi-def") internet radio is almost always FLAC inside an Ogg
 * container, served by Icecast as "audio/ogg" just like Vorbis.  The FLAC
 * decoder module only reads native FLAC, so this converts the Ogg stream
 * into one on the fly, without libogg:
 *
 *   - the first packet of the stream (Ogg FLAC mapping header) is
 *     0x7F "FLAC" major minor count(2) followed by "fLaC" and the STREAMINFO
 *     block; dropping its 9-byte prefix leaves the native stream header,
 *   - the following header packets are FLAC metadata blocks, kept verbatim,
 *   - every audio packet is one FLAC frame (starts 0xFF 0xF8/0xF9), kept
 *     verbatim.
 *
 * Icecast sources begin a new chained logical stream (new serial, headers
 * repeated) at every track change.  Repeating "fLaC" mid-stream would break
 * the decoder, so later chains contribute only their audio frames, and only
 * while their sample rate, channels and bit depth match the first chain's;
 * a format change ends the stream.  Pages of any other logical stream are
 * skipped and a damaged page is resynchronised on the next "OggS".
 *
 * Page CRCs are not checked: the stream already arrived over TCP. */

#define RADIO_OGGFLAC_PAGE_MAX 65025UL /* 255 lacing values of 255 bytes */

#define RADIO_SNIFF_UNKNOWN     0
#define RADIO_SNIFF_OGG_FLAC    1
#define RADIO_SNIFF_NATIVE_FLAC 2
#define RADIO_SNIFF_OGG_VORBIS  3
#define RADIO_SNIFF_BYTES       64 /* enough for the first page's header */

typedef size_t (*RadioOggFlacReadFn)(void *ctx, unsigned char *dst, size_t bytes);

typedef struct RadioOggFlac {
    unsigned char *page;        /* RADIO_OGGFLAC_PAGE_MAX bytes, caller-owned */
    unsigned long outPos;
    unsigned long outLen;
    unsigned long serial;
    unsigned long skip;         /* prefix bytes still to drop from this packet */
    unsigned char format[4];    /* first chain's STREAMINFO rate/channels/bits */
    int haveSerial;
    int chains;
    int inPacket;
    int emitPacket;
    int audioStarted;
    int eof;
} RadioOggFlac;

/* Classifies the first bytes of a stream. */
static int radio_oggflac_sniff(const unsigned char *b, size_t n)
{
    size_t body;

    if (!b) return RADIO_SNIFF_UNKNOWN;
    if (n >= 4 && memcmp(b, "fLaC", 4) == 0) return RADIO_SNIFF_NATIVE_FLAC;
    if (n < 28 || memcmp(b, "OggS", 4) != 0) return RADIO_SNIFF_UNKNOWN;
    body = 27 + (size_t)b[26];
    if (n >= body + 5 && memcmp(b + body, "\177FLAC", 5) == 0) return RADIO_SNIFF_OGG_FLAC;
    if (n >= body + 7 && memcmp(b + body, "\001vorbis", 7) == 0) return RADIO_SNIFF_OGG_VORBIS;
    return RADIO_SNIFF_UNKNOWN;
}

static void radio_oggflac_init(RadioOggFlac *u, unsigned char *pageBuf)
{
    memset(u, 0, sizeof(*u));
    u->page = pageBuf;
}

static int radio_oggflac_read_exact(RadioOggFlacReadFn fn, void *ctx,
                                    unsigned char *dst, unsigned long n)
{
    while (n > 0) {
        size_t got = fn(ctx, dst, (size_t)n);
        if (got == 0) return 0;
        dst += got;
        n -= (unsigned long)got;
    }
    return 1;
}

/* Reads pages until one yields native FLAC bytes (left in u->page[0..outLen)).
 * Returns 0 at end of stream. */
static int radio_oggflac_next_page(RadioOggFlac *u, RadioOggFlacReadFn fn, void *ctx)
{
    unsigned char hdr[27];
    unsigned char lace[255];

    for (;;) {
        unsigned long payload = 0;
        unsigned long rd = 0;
        unsigned long wr = 0;
        unsigned long serial;
        int nsegs, bos, cont, i;

        if (!radio_oggflac_read_exact(fn, ctx, hdr, 4)) return 0;
        while (memcmp(hdr, "OggS", 4) != 0) {
            memmove(hdr, hdr + 1, 3);
            if (!radio_oggflac_read_exact(fn, ctx, hdr + 3, 1)) return 0;
        }
        if (!radio_oggflac_read_exact(fn, ctx, hdr + 4, 23)) return 0;
        nsegs = hdr[26];
        if (!radio_oggflac_read_exact(fn, ctx, lace, (unsigned long)nsegs)) return 0;
        for (i = 0; i < nsegs; i++) payload += lace[i];
        if (!radio_oggflac_read_exact(fn, ctx, u->page, payload)) return 0;

        serial = (unsigned long)hdr[14] | ((unsigned long)hdr[15] << 8) |
                 ((unsigned long)hdr[16] << 16) | ((unsigned long)hdr[17] << 24);
        cont = hdr[5] & 1;
        bos = hdr[5] & 2;
        if (bos) {
            /* 9 mapping bytes + "fLaC" + 4-byte block header, then STREAMINFO,
             * whose bytes 10..13 carry rate(20) channels(3) bits(5). */
            unsigned char format[4];
            if (payload < 31 || memcmp(u->page, "\177FLAC", 5) != 0) {
                /* Another codec's logical stream: skip one grouped with ours
                 * at the start; a chain switching codec ends the stream. */
                if (u->audioStarted) return 0;
                continue;
            }
            memcpy(format, u->page + 27, 4);
            format[3] &= 0xf0;
            if (!u->haveSerial)
                memcpy(u->format, format, 4);
            else if (memcmp(u->format, format, 4) != 0)
                return 0;
            u->serial = serial;
            u->haveSerial = 1;
            u->chains++;
            u->inPacket = 0;
        } else if (!u->haveSerial || serial != u->serial) {
            continue;
        }

        for (i = 0; i < nsegs; i++) {
            unsigned long len = lace[i];
            unsigned long from = rd;

            if (i == 0 && cont) {
                if (!u->inPacket) u->emitPacket = 0;    /* tail of an unseen packet */
            } else if (i == 0 || !u->inPacket) {
                /* Packet start. Metadata blocks never have type 127, so a
                 * first byte of 0xFF is always a frame sync. */
                u->skip = 0;
                if (bos) {
                    u->emitPacket = u->chains == 1;
                    u->skip = 9;
                } else if (len > 0 && u->page[rd] == 0xff) {
                    u->emitPacket = 1;
                    u->audioStarted = 1;
                } else {
                    u->emitPacket = len > 0 && u->chains == 1 && !u->audioStarted;
                }
            }
            u->inPacket = len == 255;
            if (u->skip > 0) {
                unsigned long s = u->skip < len ? u->skip : len;
                from += s;
                u->skip -= s;
            }
            if (u->emitPacket && rd + len > from) {
                if (wr != from) memmove(u->page + wr, u->page + from, (size_t)(rd + len - from));
                wr += rd + len - from;
            }
            rd += len;
        }
        if (wr > 0) {
            u->outPos = 0;
            u->outLen = wr;
            return 1;
        }
    }
}

/* Reads native FLAC bytes converted from the Ogg FLAC stream fn reads.
 * Like a socket read it may return fewer bytes than asked; 0 means end. */
static size_t radio_oggflac_read(RadioOggFlac *u, RadioOggFlacReadFn fn, void *ctx,
                                 unsigned char *dest, size_t bytes)
{
    size_t done = 0;

    while (done < bytes) {
        unsigned long take;

        if (u->outPos >= u->outLen) {
            if (done > 0 || u->eof) break;
            if (!radio_oggflac_next_page(u, fn, ctx)) {
                u->eof = 1;
                break;
            }
        }
        take = u->outLen - u->outPos;
        if (take > (unsigned long)(bytes - done)) take = (unsigned long)(bytes - done);
        memcpy(dest + done, u->page + u->outPos, (size_t)take);
        u->outPos += take;
        done += (size_t)take;
    }
    return done;
}

#endif
