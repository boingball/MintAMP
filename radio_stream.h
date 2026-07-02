#ifndef RADIO_STREAM_H
#define RADIO_STREAM_H

#ifndef ENABLE_RADIO
#define ENABLE_RADIO 0
#endif

#ifndef RADIO_DEBUG_STOP
#define RADIO_DEBUG_STOP 0
#endif
#include "radio_debug.h"
#if defined(RADIO_DEBUG) || RADIO_DEBUG_STOP
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
/* Open the process-wide network libraries (bsdsocket.library + AmiSSL)
 * exactly once, at application startup.  Safe to call more than once (a
 * no-op if already open) and safe even if the caller never uses HTTPS.
 * Every probe/browser/stream connection still opens and closes its own
 * socket and, for HTTPS, its own SSL/SSL_CTX -- only the shared libraries
 * stay open for the app's lifetime instead of being reopened per request. */
void Radio_NetworkInit(void);
/* Release the process-wide network libraries (AmiSSL master + bsdsocket.library)
 * exactly once, at application exit.  Call only after every playback child has
 * been stopped and reaped.  Safe to call when nothing was ever opened. */
void Radio_NetworkShutdown(void);
void Radio_GetNetworkStats(long *active_stream_sessions, long *active_stream_tasks,
    long *open_socket_count, long *active_ssl_count, long *active_ssl_ctx_count);
void Radio_GetNetworkBases(void **socket_base, void **amissl_base, void **amissl_master_base);
/* Shared AmiSSL instance opened by Radio_NetworkInit()/radio_stream.c, for
 * radio_stream_probe.c to adopt instead of opening a second instance (its
 * weak-symbol copies of the bases do not reliably merge with the strong
 * definitions under the m68k hunk linker).  All NULL when not open. */
void Radio_GetAmiSslShared(void **amissl_base, void **amissl_ext_base, void **amissl_master_base);
int Radio_IsMemoryPoisoned(void);
void Radio_MarkMemoryPoisoned(const char *where);
int Radio_IsTlsPoisoned(void);
void Radio_MarkTlsPoisoned(const char *where);
/* Record a fatal-but-survivable TLS fault (SSL_ERROR_SSL from SSL_read):
 * the failing session is torn down normally and HTTPS stays enabled.
 * Repeated faults in one run blow a fuse and hard-poison HTTPS. */
void Radio_ReportTlsFault(const char *where);
const char *Radio_TlsPoisonedMessage(void);
/* First (root-cause) reason AmiSSL was marked poisoned this run, or
 * "not-poisoned". */
const char *Radio_TlsPoisonReason(void);
int Radio_CheckMiniMem(const char *where);
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
static void Radio_NetworkInit(void) { }
static void Radio_NetworkShutdown(void) { }
static void Radio_GetNetworkStats(long *active_stream_sessions, long *active_stream_tasks,
    long *open_socket_count, long *active_ssl_count, long *active_ssl_ctx_count)
{
    if (active_stream_sessions) *active_stream_sessions = 0;
    if (active_stream_tasks) *active_stream_tasks = 0;
    if (open_socket_count) *open_socket_count = 0;
    if (active_ssl_count) *active_ssl_count = 0;
    if (active_ssl_ctx_count) *active_ssl_ctx_count = 0;
}
static void Radio_GetNetworkBases(void **socket_base, void **amissl_base, void **amissl_master_base)
{
    if (socket_base) *socket_base = 0;
    if (amissl_base) *amissl_base = 0;
    if (amissl_master_base) *amissl_master_base = 0;
}
static void Radio_GetAmiSslShared(void **amissl_base, void **amissl_ext_base, void **amissl_master_base)
{
    if (amissl_base) *amissl_base = 0;
    if (amissl_ext_base) *amissl_ext_base = 0;
    if (amissl_master_base) *amissl_master_base = 0;
}
static int Radio_IsMemoryPoisoned(void) { return 0; }
static void Radio_MarkMemoryPoisoned(const char *where) { (void)where; }
static int Radio_IsTlsPoisoned(void) { return 0; }
static void Radio_MarkTlsPoisoned(const char *where) { (void)where; }
static void Radio_ReportTlsFault(const char *where) { (void)where; }
static const char *Radio_TlsPoisonedMessage(void) { return "HTTPS disabled after fatal TLS/memory faults; reboot before using HTTPS."; }
static const char *Radio_TlsPoisonReason(void) { return "not-poisoned"; }
static int Radio_CheckMiniMem(const char *where) { (void)where; return 0; }
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
