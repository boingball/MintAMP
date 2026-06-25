#ifndef RADIO_STREAM_H
#define RADIO_STREAM_H

typedef struct RadioStream RadioStream;

typedef enum {
    RADIO_STATUS_IDLE,
    RADIO_STATUS_CONNECTING,
    RADIO_STATUS_BUFFERING,
    RADIO_STATUS_PLAYING,
    RADIO_STATUS_RECONNECTING,
    RADIO_STATUS_ERROR
} RadioStatus;

RadioStream *Radio_Open(const char *url);
void Radio_Close(RadioStream *rs);
int Radio_Pump(RadioStream *rs);
int Radio_ReadAudio(RadioStream *rs, unsigned char *buf, int maxBytes);
RadioStatus Radio_GetStatus(RadioStream *rs);
const char *Radio_GetTitle(RadioStream *rs);
const char *Radio_GetContentType(RadioStream *rs);
const char *Radio_GetError(RadioStream *rs);
int Radio_GetBitrate(RadioStream *rs);
int Radio_GetBufferedBytes(RadioStream *rs);
const char *Radio_StatusText(RadioStatus status);

#endif
