#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../radio_oggflac.h"

/* Builds synthetic Ogg FLAC streams (CRC fields left zero: the converter
 * does not check them) and checks the native FLAC stream it produces. */

static unsigned char stream[200000];
static size_t streamLen;

typedef struct Source {
    const unsigned char *data;
    size_t len;
    size_t pos;
    size_t chunk; /* maximum bytes per read, like a socket */
} Source;

static size_t source_read(void *ctx, unsigned char *dst, size_t bytes)
{
    Source *s = (Source *)ctx;
    size_t n = s->len - s->pos;
    if (n > bytes) n = bytes;
    if (n > s->chunk) n = s->chunk;
    memcpy(dst, s->data + s->pos, n);
    s->pos += n;
    return n;
}

static void put(const void *p, size_t n)
{
    assert(streamLen + n <= sizeof(stream));
    memcpy(stream + streamLen, p, n);
    streamLen += n;
}

/* One page holding the given packets; the last may be left open
 * (continued on the next page) and the first may continue a packet. */
static void page(unsigned long serial, int flags, const unsigned char **pkts,
                 const size_t *lens, int count, int lastOpen)
{
    unsigned char hdr[27];
    unsigned char lace[255];
    int nsegs = 0;
    int i;

    for (i = 0; i < count; i++) {
        size_t n = lens[i];
        while (n >= 255) { lace[nsegs++] = 255; n -= 255; }
        if (!(lastOpen && i == count - 1)) lace[nsegs++] = (unsigned char)n;
        else assert(n == 0);
    }
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, "OggS", 4);
    hdr[5] = (unsigned char)flags;
    hdr[14] = (unsigned char)serial;
    hdr[15] = (unsigned char)(serial >> 8);
    hdr[16] = (unsigned char)(serial >> 16);
    hdr[17] = (unsigned char)(serial >> 24);
    hdr[26] = (unsigned char)nsegs;
    put(hdr, 27);
    put(lace, (size_t)nsegs);
    for (i = 0; i < count; i++) put(pkts[i], lens[i]);
}

static void page1(unsigned long serial, int flags, const unsigned char *p, size_t n)
{
    page(serial, flags, &p, &n, 1, 0);
}

/* Mapping header + STREAMINFO with the given rate (20 bits). */
static size_t bos_packet(unsigned char *out, unsigned long rate)
{
    static const unsigned char head[] = { 0x7f, 'F', 'L', 'A', 'C', 1, 0, 0, 1,
                                          'f', 'L', 'a', 'C', 0x00, 0, 0, 34 };
    memcpy(out, head, sizeof(head));
    memset(out + 17, 0x11, 34);
    out[27] = (unsigned char)(rate >> 12);
    out[28] = (unsigned char)(rate >> 4);
    out[29] = (unsigned char)(((rate & 15) << 4) | (1 << 1)); /* 2 ch */
    out[30] = (unsigned char)(0xf0 | 0x05);                    /* 16 bit */
    return 51;
}

static size_t frame(unsigned char *out, size_t n, unsigned char fill)
{
    out[0] = 0xff;
    out[1] = 0xf8;
    memset(out + 2, fill, n - 2);
    return n;
}

static size_t convert(unsigned char *out, size_t outSize, size_t chunk)
{
    static unsigned char pageBuf[RADIO_OGGFLAC_PAGE_MAX];
    RadioOggFlac u;
    Source src;
    size_t total = 0;

    src.data = stream;
    src.len = streamLen;
    src.pos = 0;
    src.chunk = chunk;
    radio_oggflac_init(&u, pageBuf);
    for (;;) {
        size_t want = outSize - total < 37 ? outSize - total : 37;
        size_t got = radio_oggflac_read(&u, source_read, &src, out + total, want);
        if (got == 0) break;
        total += got;
    }
    return total;
}

int main(void)
{
    static unsigned char expect[200000];
    static unsigned char out[200000];
    unsigned char bos[51], bos2[51], comment[20], f1[100], f2[700], f3[300], f4[50];
    const unsigned char *pk[2];
    size_t ln[2];
    size_t expectLen = 0, n, chunk;

    bos_packet(bos, 44100);
    comment[0] = 0x84; comment[1] = 0; comment[2] = 0; comment[3] = 16;
    memset(comment + 4, 'c', 16);
    frame(f1, sizeof(f1), 1);
    frame(f2, sizeof(f2), 2);
    frame(f3, sizeof(f3), 3);
    frame(f4, sizeof(f4), 4);

    /* Junk before the first page, a foreign stream's BOS, our headers,
     * a frame split across two pages, a foreign page, then a new chain
     * (same format) whose headers must be dropped. */
    put("junk!", 5);
    {
        unsigned char vorbis[30] = { 1, 'v', 'o', 'r', 'b', 'i', 's' };
        page1(99, 2, vorbis, sizeof(vorbis));
    }
    page1(7, 2, bos, sizeof(bos));
    page1(7, 0, comment, sizeof(comment));
    pk[0] = f1; ln[0] = sizeof(f1);
    pk[1] = f2; ln[1] = 510;                 /* first 510 bytes of f2 */
    page(7, 0, pk, ln, 2, 1);
    pk[0] = f2 + 510; ln[0] = sizeof(f2) - 510;
    page(7, 1, pk, ln, 1, 0);
    page1(99, 0, f4, sizeof(f4));
    bos_packet(bos2, 44100);
    page1(8, 2, bos2, sizeof(bos2));
    page1(8, 0, comment, sizeof(comment));
    page1(8, 0, f3, sizeof(f3));

    memcpy(expect + expectLen, bos + 9, 42); expectLen += 42;
    memcpy(expect + expectLen, comment, sizeof(comment)); expectLen += sizeof(comment);
    memcpy(expect + expectLen, f1, sizeof(f1)); expectLen += sizeof(f1);
    memcpy(expect + expectLen, f2, sizeof(f2)); expectLen += sizeof(f2);
    memcpy(expect + expectLen, f3, sizeof(f3)); expectLen += sizeof(f3);

    for (chunk = 1; chunk <= 4096; chunk = chunk * 3 + 1) {
        n = convert(out, sizeof(out), chunk);
        assert(n == expectLen);
        assert(memcmp(out, expect, n) == 0);
    }
    assert(memcmp(out, "fLaC", 4) == 0);

    /* A chain changing sample rate ends the stream after the old frames. */
    {
        size_t keep = streamLen;
        bos_packet(bos2, 96000);
        page1(9, 2, bos2, sizeof(bos2));
        page1(9, 0, f4, sizeof(f4));
        n = convert(out, sizeof(out), 512);
        assert(n == expectLen);
        streamLen = keep;
    }

    /* A chain switching codec after audio started also ends it. */
    {
        unsigned char vorbis[30] = { 1, 'v', 'o', 'r', 'b', 'i', 's' };
        page1(10, 2, vorbis, sizeof(vorbis));
        page1(10, 0, f4, sizeof(f4));
        n = convert(out, sizeof(out), 512);
        assert(n == expectLen);
    }

    /* Sniffing. */
    {
        unsigned char b[64];
        memset(b, 0, sizeof(b));
        memcpy(b, "OggS", 4);
        b[26] = 1;
        b[27] = 51;
        memcpy(b + 28, bos, 36);
        assert(radio_oggflac_sniff(b, sizeof(b)) == RADIO_SNIFF_OGG_FLAC);
        memcpy(b + 28, "\001vorbis", 7);
        assert(radio_oggflac_sniff(b, sizeof(b)) == RADIO_SNIFF_OGG_VORBIS);
        memcpy(b + 28, "OpusHead", 8);
        assert(radio_oggflac_sniff(b, sizeof(b)) == RADIO_SNIFF_UNKNOWN);
        assert(radio_oggflac_sniff((const unsigned char *)"fLaC\0\0\0\042", 8) == RADIO_SNIFF_NATIVE_FLAC);
        assert(radio_oggflac_sniff((const unsigned char *)"\377\373\220\144", 4) == RADIO_SNIFF_UNKNOWN);
    }

    printf("radio_oggflac_test: ok\n");
    return 0;
}
