/*
 * wma_asf.c - minimal ASF container reader (see wma_asf.h).
 *
 * Byte offsets and field layout below were cross-checked against Rockbox's
 * lib/rbcodec/metadata/asf.c (asf_parse_header) and
 * lib/rbcodec/codecs/libasf/asf.c (asf_read_packet) -- ASF's structure is
 * a straightforward GUID+size object tree (like RIFF/IFF chunks but with
 * 16-byte GUIDs instead of 4-byte FourCCs), so this is an original,
 * from-scratch implementation adapted to this project's DecoderReadCb/
 * DecoderSeekCb I/O model rather than a line-for-line port -- Rockbox's
 * own parser is split across two files tied to its buffered-file/id3
 * abstractions (ci->read_filebuf, id3->toc) that don't apply here.
 */

#include "wma_asf.h"
#include "wma_alloc.h"

/* --- GUIDs (little-endian on disk; compared byte-for-byte here) -------- */

typedef struct AsfGuid {
    unsigned long  v1;
    unsigned short v2;
    unsigned short v3;
    unsigned char  v4[8];
} AsfGuid;

static const AsfGuid kGuidHeader =
    {0x75B22630UL, 0x668E, 0x11CF, {0xA6,0xD9,0x00,0xAA,0x00,0x62,0xCE,0x6C}};
static const AsfGuid kGuidData =
    {0x75B22636UL, 0x668E, 0x11CF, {0xA6,0xD9,0x00,0xAA,0x00,0x62,0xCE,0x6C}};
static const AsfGuid kGuidFileProperties =
    {0x8CABDCA1UL, 0xA947, 0x11CF, {0x8E,0xE4,0x00,0xC0,0x0C,0x20,0x53,0x65}};
static const AsfGuid kGuidStreamProperties =
    {0xB7DC0791UL, 0xA9B7, 0x11CF, {0x8E,0xE6,0x00,0xC0,0x0C,0x20,0x53,0x65}};
static const AsfGuid kGuidStreamTypeAudio =
    {0xF8699E40UL, 0x5B4D, 0x11CF, {0xA8,0xFD,0x00,0x80,0x5F,0x5C,0x44,0x2B}};

static int GuidEqual(const AsfGuid *a, const AsfGuid *b)
{
    int i;
    if (a->v1 != b->v1 || a->v2 != b->v2 || a->v3 != b->v3)
        return 0;
    for (i = 0; i < 8; i++)
        if (a->v4[i] != b->v4[i])
            return 0;
    return 1;
}

/* --- Raw I/O helpers (mirrors wav_module.c / iff_module.c) ------------- */

static int ReadExactRaw(DecoderReadCb readFn, void *userData,
                         unsigned char *buf, unsigned long n)
{
    unsigned long got = 0;
    while (got < n) {
        DecLong r = readFn(userData, buf + got, n - got);
        if (r <= 0)
            return 0;
        got += (unsigned long)r;
    }
    return 1;
}

static int SkipRaw(DecoderReadCb readFn, void *userData, unsigned long n)
{
    unsigned char scratch[64];
    while (n > 0) {
        unsigned long want = (n > sizeof(scratch)) ? (unsigned long)sizeof(scratch) : n;
        DecLong r = readFn(userData, scratch, want);
        if (r <= 0)
            return 0;
        n -= (unsigned long)r;
    }
    return 1;
}

static unsigned short LE16(const unsigned char *p)
{
    return (unsigned short)((unsigned short)p[0] | ((unsigned short)p[1] << 8));
}

static unsigned long LE32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static unsigned long long LE64(const unsigned char *p)
{
    return (unsigned long long)LE32(p) |
           ((unsigned long long)LE32(p + 4) << 32);
}

static int ReadGuid(DecoderReadCb readFn, void *userData, AsfGuid *g, unsigned long *pos)
{
    unsigned char buf[16];
    if (!ReadExactRaw(readFn, userData, buf, 16))
        return 0;
    g->v1 = LE32(buf);
    g->v2 = LE16(buf + 4);
    g->v3 = LE16(buf + 6);
    WmaMemcpy(g->v4, buf + 8, 8);
    *pos += 16;
    return 1;
}

static int ReadU16(DecoderReadCb readFn, void *userData, unsigned short *out, unsigned long *pos)
{
    unsigned char b[2];
    if (!ReadExactRaw(readFn, userData, b, 2))
        return 0;
    *out = LE16(b);
    *pos += 2;
    return 1;
}

static int ReadU32(DecoderReadCb readFn, void *userData, unsigned long *out, unsigned long *pos)
{
    unsigned char b[4];
    if (!ReadExactRaw(readFn, userData, b, 4))
        return 0;
    *out = LE32(b);
    *pos += 4;
    return 1;
}

static int ReadU64(DecoderReadCb readFn, void *userData, unsigned long long *out, unsigned long *pos)
{
    unsigned char b[8];
    if (!ReadExactRaw(readFn, userData, b, 8))
        return 0;
    *out = LE64(b);
    *pos += 8;
    return 1;
}

static int Skip(DecoderReadCb readFn, void *userData, unsigned long n, unsigned long *pos)
{
    if (n == 0)
        return 1;
    if (!SkipRaw(readFn, userData, n))
        return 0;
    *pos += n;
    return 1;
}

/* --- Demux state --------------------------------------------------------- */

/* ASF Packet Length Type / Padding Length Type / Sequence Type field
 * widths: 0=absent(0 bytes,value 0) 1=BYTE(1) 2=WORD(2) 3=DWORD(4). */
static unsigned long Get2bLen(unsigned int bits)
{
    return (bits == 3) ? 4UL : (unsigned long)bits;
}

static unsigned long Get2bValue(unsigned int bits, const unsigned char *p)
{
    if (bits == 0) return 0;
    if (bits == 1) return p[0];
    if (bits == 2) return LE16(p);
    return LE32(p);
}

struct WmaAsfDemux {
    DecoderReadCb readFn;
    DecoderSeekCb seekFn;
    void         *userData;

    asf_waveformatex_t wfx;

    unsigned char *packetBuf;    /* holds one raw Data Packet, size wfx.packet_size */
    unsigned char *audioBuf;     /* compacted audio-stream payload for the packet */
    unsigned long  audioBufCap;  /* logical bound = wfx.packet_size, used to
                                   * reject malformed/oversized packets */
    unsigned long  audioBufAlloc;/* real allocation size, >= audioBufCap */
};

/*
 * The vendored WMA decoder core (decoders/wma/wmadeci.c
 * wma_decode_superframe_frame(), bit-reservoir path) deliberately inits
 * its bitstream reader against MAX_CODED_SUPERFRAME_SIZE (16384) bytes
 * from the caller-supplied buffer regardless of the real payload length --
 * upstream relies on VLC/Huffman decoding self-terminating within the
 * genuine data before the "extra" bytes matter, not on a hard buffer
 * boundary, so the buffer only needs to be that large, not exactly sized.
 * audioBuf must be allocated with this much headroom or the decoder reads
 * past the end of it (caught by a host-side valgrind run against a
 * synthetic small packet_size; wmadec.h's own MAX_CODED_SUPERFRAME_SIZE
 * isn't pulled in here to avoid coupling this general ASF layer to the
 * WMA decoder's internals for one constant, but it must stay in sync).
 */
#define WMA_ASF_AUDIOBUF_SAFETY_PAD 16384UL

/*
 * Parse the ASF Header Object: locate the File Properties Object (for
 * packet_size) and the first WMAV1/WMAV2 Stream Properties Object (for
 * wfx), then position the stream at the first Data Packet.
 */
static int ParseHeader(DecoderReadCb readFn, DecoderSeekCb seekFn, void *userData,
                        asf_waveformatex_t *wfx)
{
    unsigned long pos = 0;
    AsfGuid g;
    unsigned long long headerSize;
    unsigned long subobjects;
    unsigned short reserved16;
    unsigned long headerEnd;

    (void)seekFn;

    if (!ReadGuid(readFn, userData, &g, &pos))
        return 0;
    if (!GuidEqual(&g, &kGuidHeader))
        return 0;
    if (!ReadU64(readFn, userData, &headerSize, &pos))
        return 0;
    if (headerSize < 30 || headerSize > 0xFFFFFFFFUL)
        return 0;
    headerEnd = (unsigned long)headerSize; /* pos is 24 here; headerSize is
                                             * measured from the start of
                                             * the Header Object (pos==0) */
    if (!ReadU32(readFn, userData, &subobjects, &pos))
        return 0;
    if (!ReadU16(readFn, userData, &reserved16, &pos)) /* reserved */
        return 0;

    wfx->audiostream = -1;
    wfx->packet_size = 0;

    {
        unsigned long i;
        for (i = 0; i < subobjects && pos + 24 <= headerEnd; i++) {
            AsfGuid   objGuid;
            unsigned long long objSizeLL;
            unsigned long objSize, objStart;

            objStart = pos;
            if (!ReadGuid(readFn, userData, &objGuid, &pos))
                return 0;
            if (!ReadU64(readFn, userData, &objSizeLL, &pos))
                return 0;
            if (objSizeLL < 24 || objSizeLL > 0xFFFFFFFFUL)
                return 0;
            objSize = (unsigned long)objSizeLL;
            /* pos is now objStart + 24, past the GUID+size header. */

            if (GuidEqual(&objGuid, &kGuidFileProperties)) {
                unsigned long long numpackets, playDuration;
                unsigned long packetSize;
                if (objSize < 104)
                    return 0;
                if (!Skip(readFn, userData, 32, &pos))
                    return 0;
                if (!ReadU64(readFn, userData, &numpackets, &pos))
                    return 0;
                if (!ReadU64(readFn, userData, &playDuration, &pos))
                    return 0;
                (void)playDuration;
                if (!Skip(readFn, userData, 20, &pos))
                    return 0;
                if (!ReadU32(readFn, userData, &packetSize, &pos))
                    return 0;
                wfx->packet_size = packetSize;
                wfx->numpackets  = numpackets;
            } else if (GuidEqual(&objGuid, &kGuidStreamProperties)) {
                AsfGuid streamType;
                unsigned long propDataLen;
                unsigned long errCorrLen4;
                unsigned short flags;

                if (objSize < 78)
                    return 0;
                if (!ReadGuid(readFn, userData, &streamType, &pos))
                    return 0;
                if (!Skip(readFn, userData, 24, &pos)) /* err-correction GUID + time offset */
                    return 0;
                if (!ReadU32(readFn, userData, &propDataLen, &pos))
                    return 0;
                if (!ReadU32(readFn, userData, &errCorrLen4, &pos)) /* err-correction data length */
                    return 0;
                (void)errCorrLen4;
                if (!ReadU16(readFn, userData, &flags, &pos))
                    return 0;

                if (GuidEqual(&streamType, &kGuidStreamTypeAudio) && wfx->audiostream == -1) {
                    unsigned short codecId, channels, blockAlign, bitsPerSample, dataLen;
                    unsigned long  rate, bitrateBytes;
                    unsigned long  extraLen;

                    if (!Skip(readFn, userData, 4, &pos)) /* reserved */
                        return 0;
                    if (propDataLen < 18)
                        return 0;

                    if (!ReadU16(readFn, userData, &codecId, &pos))     return 0;
                    if (!ReadU16(readFn, userData, &channels, &pos))    return 0;
                    if (!ReadU32(readFn, userData, &rate, &pos))        return 0;
                    if (!ReadU32(readFn, userData, &bitrateBytes, &pos)) return 0;
                    if (!ReadU16(readFn, userData, &blockAlign, &pos))  return 0;
                    if (!ReadU16(readFn, userData, &bitsPerSample, &pos)) return 0;
                    if (!ReadU16(readFn, userData, &dataLen, &pos))     return 0;

                    wfx->codec_id      = codecId;
                    wfx->channels      = channels;
                    wfx->rate          = rate;
                    wfx->bitrate       = bitrateBytes * 8UL;
                    wfx->blockalign    = blockAlign;
                    wfx->bitspersample = bitsPerSample;
                    wfx->datalen       = dataLen;

                    extraLen = 0;
                    if (codecId == ASF_CODEC_ID_WMAV1)
                        extraLen = 4;
                    else if (codecId == ASF_CODEC_ID_WMAV2)
                        extraLen = 6;

                    if (extraLen > sizeof(wfx->data))
                        return 0; /* can't happen for v1/v2, guards the buffer */

                    if (extraLen > 0) {
                        if (!ReadExactRaw(readFn, userData, wfx->data, extraLen))
                            return 0;
                        pos += extraLen;
                    }
                    if (codecId == ASF_CODEC_ID_WMAV1 || codecId == ASF_CODEC_ID_WMAV2)
                        wfx->audiostream = flags & 0x7f;
                    /* else: WMAPro/WMAVoice/MP3-in-ASF/other -- out of scope
                     * for this decoder (classic WMA v1/v2 only); leave
                     * audiostream at -1 so ParseHeader keeps looking, and
                     * final validation below rejects the file if none of
                     * the streams turn out to be WMAV1/V2. */
                }
            }

            /* Skip to the end of this sub-object regardless of how much of
             * it we actually parsed above.  objSize is measured from
             * objStart (it includes the 24-byte GUID+size header we just
             * consumed), not from objDataStart -- do not add objSize to
             * objDataStart, that double-counts the header. */
            if (objStart + objSize < pos)
                return 0; /* malformed: object claims to be smaller than what we read */
            if (!Skip(readFn, userData, objStart + objSize - pos, &pos))
                return 0;
        }
    }

    /* Land exactly at the end of the Header Object regardless of any
     * per-subobject accounting slop above. */
    if (pos > headerEnd)
        return 0;
    if (!Skip(readFn, userData, headerEnd - pos, &pos))
        return 0;

    if (wfx->audiostream < 0 || wfx->packet_size == 0)
        return 0;
    if (wfx->codec_id != ASF_CODEC_ID_WMAV1 && wfx->codec_id != ASF_CODEC_ID_WMAV2)
        return 0;
    if (wfx->channels < 1 || wfx->channels > 2 || wfx->rate == 0)
        return 0;

    /* Next top-level object must be the Data Object. */
    if (!ReadGuid(readFn, userData, &g, &pos))
        return 0;
    if (!GuidEqual(&g, &kGuidData))
        return 0;
    {
        unsigned long long dataObjSize;
        unsigned long long totalDataPackets;
        unsigned short dataReserved;
        if (!ReadU64(readFn, userData, &dataObjSize, &pos))
            return 0;
        (void)dataObjSize;
        if (!Skip(readFn, userData, 16, &pos)) /* File ID GUID */
            return 0;
        if (!ReadU64(readFn, userData, &totalDataPackets, &pos))
            return 0;
        (void)totalDataPackets;
        if (!ReadU16(readFn, userData, &dataReserved, &pos))
            return 0;
        (void)dataReserved;
    }
    /* Stream is now positioned at the first Data Packet. */
    return 1;
}

WmaAsfDemux *WmaAsfOpen(DecoderReadCb readFn, DecoderSeekCb seekFn,
                        void *userData, asf_waveformatex_t *wfxOut)
{
    WmaAsfDemux *dx;

    dx = (WmaAsfDemux *)WmaModuleCalloc(1, sizeof(WmaAsfDemux));
    if (!dx)
        return NULL;
    dx->readFn   = readFn;
    dx->seekFn   = seekFn;
    dx->userData = userData;

    if (!ParseHeader(readFn, seekFn, userData, &dx->wfx)) {
        WmaModuleFree(dx);
        return NULL;
    }

    dx->audioBufAlloc = dx->wfx.packet_size;
    if (dx->audioBufAlloc < WMA_ASF_AUDIOBUF_SAFETY_PAD)
        dx->audioBufAlloc = WMA_ASF_AUDIOBUF_SAFETY_PAD;

    dx->packetBuf = (unsigned char *)WmaModuleCalloc(1, dx->wfx.packet_size);
    dx->audioBuf  = (unsigned char *)WmaModuleCalloc(1, dx->audioBufAlloc);
    if (!dx->packetBuf || !dx->audioBuf) {
        if (dx->packetBuf) WmaModuleFree(dx->packetBuf);
        if (dx->audioBuf)  WmaModuleFree(dx->audioBuf);
        WmaModuleFree(dx);
        return NULL;
    }
    dx->audioBufCap = dx->wfx.packet_size;

    /* A plain struct assignment here (*wfxOut = dx->wfx;) is large enough
     * that GCC can lower it to a compiler-generated call to the real
     * memcpy() -- invisible to the -Dmemcpy=WmaMemcpy redirect used for
     * the vendored decoders/wma/ sources, since that only rewrites
     * explicit textual memcpy(...) calls in source, not codegen the
     * compiler inserts on its own. Spell it out explicitly instead. */
    WmaMemcpy(wfxOut, &dx->wfx, sizeof(*wfxOut));
    return dx;
}

void WmaAsfClose(WmaAsfDemux *dx)
{
    if (!dx)
        return;
    if (dx->packetBuf) WmaModuleFree(dx->packetBuf);
    if (dx->audioBuf)  WmaModuleFree(dx->audioBuf);
    WmaModuleFree(dx);
}

/*
 * WmaAsfReadPacket() -- adapted from Rockbox's libasf asf_read_packet(),
 * ported from its ci->read_filebuf()/ci->request_buffer() zero-copy ring
 * buffer onto this project's DecoderReadCb, and reading the whole packet
 * into an owned buffer up front rather than deferring the skip of
 * trailing padding to a later ci->advance_buffer() call (there's no
 * equivalent deferred-skip step needed here since the whole packet is
 * already consumed from the stream by the time this returns).
 */
static int ReadOnePacket(WmaAsfDemux *dx, const unsigned char **bufOut, unsigned long *lenOut)
{
    unsigned char hdr8;
    unsigned char packetFlags, packetProperty;
    unsigned long ecLen;
    unsigned long fixedLen;
    unsigned char fixed[18];
    const unsigned char *fp;
    unsigned long length;
    unsigned long paddingLength;
    unsigned long bytesread;
    unsigned short payloadCount;
    unsigned int   payloadLengthType;
    unsigned long  packetDataLen; /* bytes remaining in packet after the
                                    * fixed multiplexing header, i.e. all
                                    * payloads + padding */
    const unsigned char *p, *pEnd;
    unsigned long audioLen;
    int i;

    *bufOut = NULL;
    *lenOut = 0;

    if (!ReadExactRaw(dx->readFn, dx->userData, &hdr8, 1))
        return -2; /* clean EOF between packets */

    if (hdr8 != 0x82)
        return -1; /* not a supported/synced ASF packet header */

    ecLen = hdr8 & 0x0f;
    if (((hdr8 >> 5) & 0x03) != 0 || ((hdr8 >> 4) & 0x01) != 0 || ecLen != 2)
        return -1;
    if (!SkipRaw(dx->readFn, dx->userData, ecLen))
        return -1;
    bytesread = 1 + ecLen;

    if (!ReadExactRaw(dx->readFn, dx->userData, &packetFlags, 1))
        return -1;
    if (!ReadExactRaw(dx->readFn, dx->userData, &packetProperty, 1))
        return -1;
    bytesread += 2;

    fixedLen = Get2bLen((packetFlags >> 1) & 0x03) +
               Get2bLen((packetFlags >> 3) & 0x03) +
               Get2bLen((packetFlags >> 5) & 0x03) + 6;
    if (fixedLen > sizeof(fixed))
        return -1;
    if (!ReadExactRaw(dx->readFn, dx->userData, fixed, fixedLen))
        return -1;
    bytesread += fixedLen;

    fp = fixed;
    length = Get2bValue((packetFlags >> 5) & 0x03, fp);
    fp += Get2bLen((packetFlags >> 5) & 0x03);
    fp += Get2bLen((packetFlags >> 1) & 0x03); /* sequence, unused */
    paddingLength = Get2bValue((packetFlags >> 3) & 0x03, fp);
    fp += Get2bLen((packetFlags >> 3) & 0x03);
    /* send_time (4) + duration (2), unused */

    if (((packetFlags >> 5) & 0x03) == 0)
        length = dx->wfx.packet_size;
    if (length < dx->wfx.packet_size) {
        paddingLength += dx->wfx.packet_size - length;
        length = dx->wfx.packet_size;
    }
    if (length > dx->wfx.packet_size)
        return -1;

    if (packetFlags & 0x01) {
        unsigned char pc;
        if (!ReadExactRaw(dx->readFn, dx->userData, &pc, 1))
            return -1;
        bytesread++;
        payloadCount = pc & 0x3f;
        payloadLengthType = (pc >> 6) & 0x03;
    } else {
        payloadCount = 1;
        payloadLengthType = 0x02;
    }

    if (length < bytesread)
        return -1;

    packetDataLen = length - bytesread;
    if (packetDataLen > dx->audioBufCap)
        return -1; /* packet_size guard sized packetBuf; shouldn't happen */
    if (!ReadExactRaw(dx->readFn, dx->userData, dx->packetBuf, packetDataLen))
        return -1;

    p = dx->packetBuf;
    pEnd = dx->packetBuf + packetDataLen;
    audioLen = 0;

    for (i = 0; i < (int)payloadCount; i++) {
        unsigned long payloadHdrLen;
        unsigned long replicatedLength;
        unsigned long payloadDataLen;
        int streamId;

        if (p >= pEnd)
            return -1;
        streamId = *p & 0x7f;
        p++;

        payloadHdrLen = Get2bLen(packetProperty & 0x03) +
                        Get2bLen((packetProperty >> 2) & 0x03) +
                        Get2bLen((packetProperty >> 4) & 0x03);
        if ((unsigned long)(pEnd - p) < payloadHdrLen)
            return -1;

        p += Get2bLen((packetProperty >> 4) & 0x03); /* media object number, unused */
        p += Get2bLen((packetProperty >> 2) & 0x03); /* media object offset, unused */
        replicatedLength = Get2bValue(packetProperty & 0x03, p);
        p += Get2bLen(packetProperty & 0x03);

        if ((unsigned long)(pEnd - p) < replicatedLength)
            return -1;
        p += replicatedLength;

        if (packetFlags & 0x01) {
            unsigned long x = Get2bLen(payloadLengthType);
            if (x != 2 || (unsigned long)(pEnd - p) < x)
                return -1;
            payloadDataLen = Get2bValue(payloadLengthType, p);
            p += x;
        } else {
            unsigned long consumedSoFar = (unsigned long)(p - dx->packetBuf) + bytesread;
            if (consumedSoFar + paddingLength > length)
                return -1;
            payloadDataLen = length - consumedSoFar - paddingLength;
        }

        if (replicatedLength == 1) {
            if (p >= pEnd)
                return -1;
            p++; /* one-byte "compressed payload" presentation-time delta */
        }

        if ((unsigned long)(pEnd - p) < payloadDataLen)
            return -1;

        if (streamId == dx->wfx.audiostream) {
            if (audioLen + payloadDataLen > dx->audioBufCap)
                return -1;
            WmaMemcpy(dx->audioBuf + audioLen, p, payloadDataLen);
            audioLen += payloadDataLen;
        }
        p += payloadDataLen;
    }

    if (audioLen == 0)
        return 0; /* no audio in this packet (e.g. a script-command-only
                    * packet); caller loops to the next one */
    *bufOut = dx->audioBuf;
    *lenOut = audioLen;
    return 1;
}

/*
 * Skip packets with no payload for our audio stream (e.g. video/script
 * command packets in a multi-stream ASF file) without recursing -- ASF
 * files can in principle contain long runs of these.  ReadOnePacket()
 * returns 1 (got audio), -2 (clean EOF), or another negative (parse
 * error); 0 means "packet consumed, no audio in it, keep going" so the
 * EOF case must use a distinct sentinel or this loop could spin forever
 * re-reading past end-of-stream.
 */
int WmaAsfReadPacket(WmaAsfDemux *dx, const unsigned char **bufOut, unsigned long *lenOut)
{
    for (;;) {
        int rc = ReadOnePacket(dx, bufOut, lenOut);
        if (rc == 1)
            return 1;
        if (rc == -2)
            return 0; /* EOF: matches decoder_module.h's "0 = EOF" */
        if (rc < 0)
            return rc;
        /* rc == 0: this packet had nothing for our stream, try the next */
    }
}
