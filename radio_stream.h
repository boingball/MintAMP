#ifndef RADIO_STREAM_H
#define RADIO_STREAM_H

#ifndef ENABLE_RADIO
#define ENABLE_RADIO 0
#endif

#ifndef RADIO_DEBUG_STOP
#define RADIO_DEBUG_STOP 0
#endif
#if RADIO_DEBUG_STOP
#include <stdio.h>
#define RADIO_STOP_DEBUG_PRINTF(x) do { printf x; } while (0)
#else
#define RADIO_STOP_DEBUG_PRINTF(x) do { } while (0)
#endif

typedef struct RadioStream RadioStream;

typedef enum {
    RADIO_STATUS_IDLE,
    RADIO_STATUS_CONNECTING,
    RADIO_STATUS_BUFFERING,
    RADIO_STATUS_PLAYING,
    RADIO_STATUS_RECONNECTING,
    RADIO_STATUS_STOPPING,
    RADIO_STATUS_CLOSED,
    RADIO_STATUS_ERROR
} RadioStatus;

#if ENABLE_RADIO
RadioStream *Radio_OpenWithHostAddr(const char *url, int haveHostAddr, unsigned long hostAddrBe);
RadioStream *Radio_Open(const char *url);
void Radio_RequestStop(RadioStream *rs);
void Radio_Close(RadioStream *rs);
int Radio_Pump(RadioStream *rs);
int Radio_ReadAudio(RadioStream *rs, unsigned char *buf, int maxBytes);
int Radio_ReadStartupAudio(RadioStream *rs, unsigned char *buf, int maxBytes, unsigned long timeoutMs);
void Radio_FailStartup(RadioStream *rs, const char *message);
RadioStatus Radio_GetStatus(RadioStream *rs);
const char *Radio_GetTitle(RadioStream *rs);
const char *Radio_GetStationName(RadioStream *rs);
const char *Radio_GetGenre(RadioStream *rs);
const char *Radio_GetStreamUrl(RadioStream *rs);
int Radio_GetMetaInt(RadioStream *rs);
const char *Radio_GetContentType(RadioStream *rs);
const char *Radio_GetError(RadioStream *rs);
int Radio_GetBitrate(RadioStream *rs);
int Radio_GetBufferedBytes(RadioStream *rs);
const char *Radio_StatusText(RadioStatus status);
/* Release the process-wide network libraries (AmiSSL master + bsdsocket.library)
 * exactly once, at application exit.  Call only after every playback child has
 * been stopped and reaped.  Safe to call when nothing was ever opened. */
void Radio_NetworkShutdown(void);
#else
static RadioStream *Radio_OpenWithHostAddr(const char *url, int haveHostAddr, unsigned long hostAddrBe) { (void)url; (void)haveHostAddr; (void)hostAddrBe; return (RadioStream *)0; }
static RadioStream *Radio_Open(const char *url) { (void)url; return (RadioStream *)0; }
static void Radio_RequestStop(RadioStream *rs) { (void)rs; }
static void Radio_Close(RadioStream *rs) { (void)rs; }
static int Radio_Pump(RadioStream *rs) { (void)rs; return -1; }
static int Radio_ReadAudio(RadioStream *rs, unsigned char *buf, int maxBytes) { (void)rs; (void)buf; (void)maxBytes; return 0; }
static int Radio_ReadStartupAudio(RadioStream *rs, unsigned char *buf, int maxBytes, unsigned long timeoutMs) { (void)rs; (void)buf; (void)maxBytes; (void)timeoutMs; return 0; }
static void Radio_FailStartup(RadioStream *rs, const char *message) { (void)rs; (void)message; }
static RadioStatus Radio_GetStatus(RadioStream *rs) { (void)rs; return RADIO_STATUS_CLOSED; }
static const char *Radio_GetTitle(RadioStream *rs) { (void)rs; return ""; }
static const char *Radio_GetStationName(RadioStream *rs) { (void)rs; return ""; }
static const char *Radio_GetGenre(RadioStream *rs) { (void)rs; return ""; }
static const char *Radio_GetStreamUrl(RadioStream *rs) { (void)rs; return ""; }
static int Radio_GetMetaInt(RadioStream *rs) { (void)rs; return 0; }
static const char *Radio_GetContentType(RadioStream *rs) { (void)rs; return ""; }
static const char *Radio_GetError(RadioStream *rs) { (void)rs; return "radio support not built"; }
static int Radio_GetBitrate(RadioStream *rs) { (void)rs; return 0; }
static int Radio_GetBufferedBytes(RadioStream *rs) { (void)rs; return 0; }
static void Radio_NetworkShutdown(void) { }
static const char *Radio_StatusText(RadioStatus status)
{
    switch (status) {
    case RADIO_STATUS_CONNECTING: return "Connecting";
    case RADIO_STATUS_BUFFERING: return "Buffering";
    case RADIO_STATUS_PLAYING: return "Playing";
    case RADIO_STATUS_RECONNECTING: return "Reconnecting";
    case RADIO_STATUS_STOPPING: return "Stopping";
    case RADIO_STATUS_CLOSED: return "Closed";
    case RADIO_STATUS_ERROR: return "Error";
    default: return "Idle";
    }
}
#endif

#endif
