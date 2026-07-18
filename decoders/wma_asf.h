/*
 * wma_asf.h - minimal ASF (Advanced Systems Format) container reader for
 * the WMA decoder module.  Original code for MintAMP (not vendored) --
 * finds the WMAV1/WMAV2 audio stream's format info and hands raw payload
 * bytes to decoders/wma/wmadeci.c (Rockbox's libwma, vendored separately).
 *
 * The asf_waveformatex_t layout matches the struct wma_decode_init() in
 * decoders/wma/wmadec.h expects (same field names/order as Rockbox's
 * lib/rbcodec/codecs/libasf/asf.h) so the vendored decoder core can be
 * used unmodified.
 */
#ifndef MINIAMP3_WMA_ASF_H
#define MINIAMP3_WMA_ASF_H

#include "decoder_module.h"

#define ASF_CODEC_ID_WMAV1      0x160
#define ASF_CODEC_ID_WMAV2      0x161
#define ASF_CODEC_ID_WMAPRO     0x162
#define ASF_CODEC_ID_WMAVOICE   0x00A
#define ASF_CODEC_ID_MP3        0x055

struct asf_waveformatex_s {
    unsigned long  packet_size;
    int            audiostream;   /* -1 until the audio stream is found  */
    unsigned short codec_id;
    unsigned short channels;
    unsigned long  rate;
    unsigned long  bitrate;       /* bits/sec (file stores bytes/sec)    */
    unsigned short blockalign;
    unsigned short bitspersample;
    unsigned short datalen;       /* valid bytes in data[]               */
    unsigned long long numpackets;
    unsigned char  data[46];      /* codec-specific extradata            */
};
typedef struct asf_waveformatex_s asf_waveformatex_t;

/* Opaque state for streaming packet-by-packet demux. */
typedef struct WmaAsfDemux WmaAsfDemux;

/*
 * WmaAsfOpen() - parse the ASF Header Object, locate the first WMAV1/WMAV2
 * audio stream's Stream Properties, and position the stream at the first
 * Data Packet.  Returns a demux handle on success (caller must free with
 * WmaAsfClose()), or NULL if this isn't a supported ASF/WMA file.
 */
WmaAsfDemux *WmaAsfOpen(DecoderReadCb readFn, DecoderSeekCb seekFn,
                        void *userData, asf_waveformatex_t *wfxOut);

/*
 * WmaAsfReadPacket() - demux the next Data Packet, returning a pointer to
 * a contiguous buffer of this packet's audio-stream payload bytes (valid
 * until the next WmaAsfReadPacket()/WmaAsfClose() call) and its length.
 * Returns 1 with *bufOut and *lenOut set on success, 0 at end of the Data
 * Object, negative on I/O/parse error.
 */
int WmaAsfReadPacket(WmaAsfDemux *dx, const unsigned char **bufOut, unsigned long *lenOut);

void WmaAsfClose(WmaAsfDemux *dx);

#endif /* MINIAMP3_WMA_ASF_H */
