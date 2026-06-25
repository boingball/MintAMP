#ifndef RADIO_STREAM_H
#define RADIO_STREAM_H

#ifndef ENABLE_RADIO
#define ENABLE_RADIO 0
#endif

typedef struct RadioStream RadioStream;

typedef enum {
    RADIO_STATUS_IDLE,
    RADIO_STATUS_CONNECTING,
    RADIO_STATUS_BUFFERING,
    RADIO_STATUS_PLAYING,
    RADIO_STATUS_RECONNECTING,
    RADIO_STATUS_ERROR
} RadioStatus;

#if ENABLE_RADIO
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
#else
static RadioStream *Radio_Open(const char *url) { (void)url; return (RadioStream *)0; }
static void Radio_Close(RadioStream *rs) { (void)rs; }
static int Radio_Pump(RadioStream *rs) { (void)rs; return -1; }
static int Radio_ReadAudio(RadioStream *rs, unsigned char *buf, int maxBytes) { (void)rs; (void)buf; (void)maxBytes; return 0; }
static RadioStatus Radio_GetStatus(RadioStream *rs) { (void)rs; return RADIO_STATUS_ERROR; }
static const char *Radio_GetTitle(RadioStream *rs) { (void)rs; return ""; }
static const char *Radio_GetContentType(RadioStream *rs) { (void)rs; return ""; }
static const char *Radio_GetError(RadioStream *rs) { (void)rs; return "radio support not built"; }
static int Radio_GetBitrate(RadioStream *rs) { (void)rs; return 0; }
static int Radio_GetBufferedBytes(RadioStream *rs) { (void)rs; return 0; }
static const char *Radio_StatusText(RadioStatus status)
{
    switch (status) {
    case RADIO_STATUS_CONNECTING: return "Connecting";
    case RADIO_STATUS_BUFFERING: return "Buffering";
    case RADIO_STATUS_PLAYING: return "Playing";
    case RADIO_STATUS_RECONNECTING: return "Reconnecting";
    case RADIO_STATUS_ERROR: return "Error";
    default: return "Idle";
    }
}
#endif

#endif
