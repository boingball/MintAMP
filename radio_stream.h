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
/* Install an app-side "stop requested" flag polled by every internal
 * pump/connect/read-audio wait loop.  Radio_RequestStop() can only be called
 * between Radio_ReadAudio() calls, so without this hook a stream stalled on a
 * dead socket (stuck "Buffering", no data, no FIN) could never observe a Stop
 * click and the playback child looped forever, deadlocking the GUI's
 * stop/close paths.  A data flag rather than a callback on purpose: the
 * radio code only ever reads through the pointer, so a stale/corrupt value
 * cannot become a wild jump.  NULL disables the poll.  On AMIGA_M68K builds
 * the same loops also honour a pending SIGBREAKF_CTRL_C on the pumping task
 * directly, no app hook needed. */
void Radio_SetStopFlag(const volatile int *flag);
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
/* For diagnostic logging only (e.g. tagging a message with which stream
 * session it belongs to from outside radio_stream.c, where RadioStream is
 * opaque).  Returns 0 for a NULL stream. */
unsigned long Radio_GetSessionId(RadioStream *rs);
/* True once a fatal TLS fault (SSL_ERROR_SSL/SYSCALL/unknown from
 * SSL_connect/SSL_read/SSL_write, or detected ring corruption) has marked
 * this session's object graph unsafe. Callers outside radio_stream.c (the
 * playback child's main()/InputSourceClose() teardown) must not free the
 * decoder context or the fast-input-memory buffer for a fatal session --
 * see Radio_Close()'s own ring-buffer/RadioStream-struct quarantine for the
 * same reasoning. Returns 0 for a NULL stream. */
int Radio_IsSessionFatal(RadioStream *rs);
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
/* Wider resource snapshot for a caller that wants to confirm every prior
 * session's resources have actually reached zero before starting a new one
 * (a queued station switch), not just that the old child Task is gone. */
void Radio_GetTeardownStats(long *active_stream_sessions, long *active_stream_tasks,
    long *open_socket_count, long *playback_open_socket_count,
    long *active_decoder_count, long *active_audio_buffer_count,
    long *active_stream_buffer_count);
void Radio_GetNetworkBases(void **socket_base, void **amissl_base, void **amissl_master_base);
/* True once Radio_NetworkInit() has successfully opened bsdsocket.library --
 * lets the GUI grey out internet-radio features up front on a machine with
 * no network stack installed, instead of failing later on first connect. */
int Radio_HasNetwork(void);
/* True once Radio_NetworkInit() has successfully opened the shared AmiSSL
 * instance -- lets the GUI grey out the HTTPS scheme option up front on a
 * machine without AmiSSL installed (always false in builds without
 * HAVE_AMISSL). */
int Radio_HasHttps(void);
int Radio_PlaybackOwnsNetwork(void);
/* Shared AmiSSL instance opened by Radio_NetworkInit()/radio_stream.c, for
 * radio_stream_probe.c to adopt instead of opening a second instance (its
 * weak-symbol copies of the bases do not reliably merge with the strong
 * definitions under the m68k hunk linker).  All NULL when not open. */
void Radio_GetAmiSslShared(void **amissl_base, void **amissl_ext_base, void **amissl_master_base);
/* True when the calling task is the one that ran OpenAmiSSLTags(): per the
 * AmiSSL v5/v6 SDK that task is already initialized and must NOT run the
 * per-subprocess InitAmiSSL()/CleanupAmiSSL() pair. */
int Radio_AmiSslTaskIsOpener(void);
/* Serializes the shared AmiSSLBase/AmiSSLExtBase/AmiSSLMasterBase/SocketBase
 * globals and their per-task "have I initialized" bookkeeping against
 * concurrent use by another task (a playback child tearing down its own
 * AmiSSL state while the GUI/opener task starts a station probe or favicon
 * fetch, or vice versa). Nestable by the same task (AmigaOS SignalSemaphore
 * semantics), so callers that already hold the lock and call back into
 * another locked helper are safe.
 *
 * Radio_AmiSslLock() is a BOUNDED, non-blocking-retry attempt (AttemptSemaphore()
 * polled with a timeout), not a plain wait: a task wedged inside AmiSSL while
 * holding this lock (this codebase has already seen CleanupAmiSSL() loop
 * forever on corrupted internals) must never turn into every other task --
 * including the one handling the user's Stop click -- blocking uninterruptibly
 * on an exec semaphore Wait() with no way to react to a signal. Returns 1 if
 * the lock was acquired, 0 if it gave up (caller proceeds unlocked, same as
 * if this lock did not exist). Only call Radio_AmiSslUnlock() when Lock()
 * returned 1 -- releasing a semaphore this task never acquired is undefined.
 * No-op (returns 0) before Radio_NetworkInit() has run and in non-Amiga
 * builds. */
int Radio_AmiSslLock(void);
void Radio_AmiSslUnlock(void);
int Radio_IsMemoryPoisoned(void);
void Radio_MarkMemoryPoisoned(const char *where);
int Radio_IsTlsPoisoned(void);
void Radio_MarkTlsPoisoned(const char *where);
/* Record a fatal-but-survivable TLS fault (SSL_ERROR_SSL from SSL_read):
 * the failing session's SSL objects are quarantined (leaked, never freed)
 * and HTTPS stays enabled -- always. Only detected memory corruption
 * hard-poisons HTTPS (Radio_MarkTlsPoisoned()). */
void Radio_ReportTlsFault(const char *where);
/* Per-run blocklist of hosts that triggered a fatal AmiSSL fault: the fault
 * can corrupt memory inside the failing AmiSSL call itself, so a known
 * offender must not be contacted over TLS again until restart. */
void Radio_NoteTlsFaultHost(const char *host);
int Radio_IsTlsFaultHost(const char *host);
const char *Radio_TlsPoisonedMessage(void);
/* First (root-cause) reason AmiSSL was marked poisoned this run, or
 * "not-poisoned". */
const char *Radio_TlsPoisonReason(void);
int Radio_CheckMiniMem(const char *where);
/* Debug builds: validate exec's memory free lists (the invariants behind
 * AN_MemCorrupt) and log OK/CORRUPT with a location tag. No-op in release. */
void Radio_DebugCheckExecMem(const char *where);
#else
static RadioStream *Radio_OpenWithHostAddr(const char *url, int haveHostAddr, unsigned long hostAddrBe) { (void)url; (void)haveHostAddr; (void)hostAddrBe; return (RadioStream *)0; }
static RadioStream *Radio_Open(const char *url) { (void)url; return (RadioStream *)0; }
static void Radio_RequestStop(RadioStream *rs) { (void)rs; }
static void Radio_SetStopFlag(const volatile int *flag) { (void)flag; }
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
static unsigned long Radio_GetSessionId(RadioStream *rs) { (void)rs; return 0; }
static int Radio_IsSessionFatal(RadioStream *rs) { (void)rs; return 0; }
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
static void Radio_GetTeardownStats(long *active_stream_sessions, long *active_stream_tasks,
    long *open_socket_count, long *playback_open_socket_count,
    long *active_decoder_count, long *active_audio_buffer_count,
    long *active_stream_buffer_count)
{
    if (active_stream_sessions) *active_stream_sessions = 0;
    if (active_stream_tasks) *active_stream_tasks = 0;
    if (open_socket_count) *open_socket_count = 0;
    if (playback_open_socket_count) *playback_open_socket_count = 0;
    if (active_decoder_count) *active_decoder_count = 0;
    if (active_audio_buffer_count) *active_audio_buffer_count = 0;
    if (active_stream_buffer_count) *active_stream_buffer_count = 0;
}
static void Radio_GetNetworkBases(void **socket_base, void **amissl_base, void **amissl_master_base)
{
    if (socket_base) *socket_base = 0;
    if (amissl_base) *amissl_base = 0;
    if (amissl_master_base) *amissl_master_base = 0;
}
static int Radio_HasNetwork(void) { return 0; }
static int Radio_HasHttps(void) { return 0; }
static int Radio_PlaybackOwnsNetwork(void) { return 0; }
static void Radio_GetAmiSslShared(void **amissl_base, void **amissl_ext_base, void **amissl_master_base)
{
    if (amissl_base) *amissl_base = 0;
    if (amissl_ext_base) *amissl_ext_base = 0;
    if (amissl_master_base) *amissl_master_base = 0;
}
static int Radio_AmiSslTaskIsOpener(void) { return 0; }
static int Radio_AmiSslLock(void) { return 0; }
static void Radio_AmiSslUnlock(void) { }
static int Radio_IsMemoryPoisoned(void) { return 0; }
static void Radio_MarkMemoryPoisoned(const char *where) { (void)where; }
static int Radio_IsTlsPoisoned(void) { return 0; }
static void Radio_MarkTlsPoisoned(const char *where) { (void)where; }
static void Radio_ReportTlsFault(const char *where) { (void)where; }
static void Radio_NoteTlsFaultHost(const char *host) { (void)host; }
static int Radio_IsTlsFaultHost(const char *host) { (void)host; return 0; }
static const char *Radio_TlsPoisonedMessage(void) { return "HTTPS disabled after memory corruption; reboot before using HTTPS."; }
static const char *Radio_TlsPoisonReason(void) { return "not-poisoned"; }
static int Radio_CheckMiniMem(const char *where) { (void)where; return 0; }
static void Radio_DebugCheckExecMem(const char *where) { (void)where; }
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
