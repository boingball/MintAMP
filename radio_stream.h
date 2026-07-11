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
#if defined(AMIGA_M68K)
/* Same cross-task stdout race as radio_debug.h's RADIO_DBG_PRINTF -- this
 * macro predates that fix and had its own unlocked printf, so it needs the
 * same lock (radio_console_lock, defined/init'd alongside RADIO_DBG). */
#include <exec/semaphores.h>
#include <proto/exec.h>
extern struct SignalSemaphore radio_console_lock;
#define RADIO_STOP_DEBUG_PRINTF(x) do { ObtainSemaphore(&radio_console_lock); printf x; fflush(stdout); ReleaseSemaphore(&radio_console_lock); } while (0)
#else
#define RADIO_STOP_DEBUG_PRINTF(x) do { printf x; } while (0)
#endif
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
/* Start the single long-lived radio net worker task (HAVE_AMISSL builds) --
 * it owns its own bsdsocket.library base, its own amisslmaster.library base,
 * its own AmiSSLBase/AmiSSLExtBase and its own AmiSSL_ErrNoPtr storage
 * privately, opens AmiSSL (OpenAmiSSLTags()/InitAmiSSL) exactly once for the
 * app's whole run, and then owns the pump loop for active stations. Open/close
 * requests still use Radio_RunOnNetWorker(), but Radio_Pump() itself is only
 * a cheap status/buffer check -- never reopening any of those bases
 * per station switch. Plain-HTTP-only m68k builds without HAVE_AMISSL just
 * open bsdsocket.library directly here instead (no per-task AmiSSL lifecycle
 * to manage). Safe to call more than once (a no-op if already up) and safe
 * even if the caller never uses HTTPS/radio at all. */
void Radio_NetworkInit(void);
/* Ask the net worker task to close its libraries (CleanupAmiSSL-adjacent
 * teardown: CloseAmiSSL()/CloseLibrary(amisslmaster)/CloseLibrary(bsdsocket),
 * exactly once, matching amissl_child_worker_repro.c) and exit, then wait,
 * bounded, for it to do so. Call only after every playback child has been
 * stopped and reaped. Safe to call when nothing was ever opened. */
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
/* The net worker task's SocketBase/AmiSSLBase/AmiSSLMasterBase are private
 * to that one task: this only ever returns non-NULL pointers when called BY
 * the worker task itself (radio_stream_probe.c's code, which now only runs
 * via Radio_RunOnNetWorker()) -- every other (GUI/opener) task always gets
 * all-NULL and must open its own independent bsdsocket.library base instead
 * (radio_browser_http.c already does this as a fallback). */
void Radio_GetNetworkBases(void **socket_base, void **amissl_base, void **amissl_master_base);
/* True once bsdsocket.library is open (the net worker task's own base in
 * HAVE_AMISSL builds, Radio_NetworkInit()'s own base otherwise) -- lets the
 * GUI grey out internet-radio features up front on a machine with no
 * network stack installed, instead of failing later on first connect. A
 * plain status flag: never exposes the SocketBase pointer itself. */
int Radio_HasNetwork(void);
/* True once the net worker task has successfully opened its AmiSSL instance
 * -- lets the GUI grey out the HTTPS scheme option up front on a machine
 * without AmiSSL installed (always false in builds without HAVE_AMISSL). */
int Radio_HasHttps(void);
int Radio_PlaybackOwnsNetwork(void);
int Radio_WorkerIsIdle(void);
const char *Radio_WorkerStateName(void);
/* Same private-to-the-worker-task rule as Radio_GetNetworkBases(): only
 * returns non-NULL to the worker task itself, letting radio_stream_probe.c
 * use the one AmiSSL instance the worker already opened instead of opening
 * a second one of its own (its weak-symbol copies of the bases do not
 * reliably merge with the strong definitions under the m68k hunk linker). */
void Radio_GetAmiSslShared(void **amissl_base, void **amissl_ext_base, void **amissl_master_base);
/* True when the calling task is the net worker task -- the only task that
 * ever runs OpenAmiSSLTags()/InitAmiSSL() in this process, so per the AmiSSL
 * v5/v6 SDK it is already initialized and must NOT run a manual
 * InitAmiSSL()/CleanupAmiSSL() pair on top of that. */
int Radio_AmiSslTaskIsOpener(void);
/* Historically serialized the shared AmiSSLBase/AmiSSLExtBase/AmiSSLMasterBase/
 * SocketBase globals against concurrent use by more than one task. With the
 * single-net-worker-task architecture only that one task ever touches those
 * globals, so there is no more concurrent access to guard: both functions
 * are now trivial (Lock always "succeeds", Unlock is a no-op), kept only so
 * radio_stream_probe.c does not need its own call-site changes. */
int Radio_AmiSslLock(void);
void Radio_AmiSslUnlock(void);
/* Run fn(arg) synchronously on the net worker task: either immediately (the
 * calling task already IS the worker -- e.g. a job calling back into
 * another helper) or by shipping the closure to the worker task over its
 * message port and blocking (bounded) for the reply. This is the only
 * sanctioned way for radio_stream_probe.c (or any other subsystem) to touch
 * bsdsocket.library/AmiSSL: no task other than the worker itself may call
 * socket()/connect()/SSL_*()/InitAmiSSL() directly. Returns 1 if fn ran
 * (on whichever task), 0 if the worker could not be started or the wait
 * timed out (the worker may be wedged inside a blocking call -- see the
 * lifecycle audit doc). No-op (returns 0) in non-Amiga/non-HAVE_AMISSL
 * builds.
 *
 * LIFETIME CONTRACT: on a timeout the queued job is detached, not cancelled
 * -- the worker may still run fn(arg) long after this call returned.  arg
 * must therefore point at heap or otherwise persistent storage, NEVER at
 * the caller's stack.  Callers with stack-local argument blocks must use
 * Radio_RunOnNetWorkerCopied() instead. */
int Radio_RunOnNetWorker(void (*fn)(void *arg), void *arg);
/* Same dispatch, but fn runs against a worker-owned heap copy of the
 * arg_size bytes at arg, so a timed-out job can never touch the caller's
 * (possibly dead) stack frame.  On success (return 1) the copy -- including
 * any results fn wrote into it -- is copied back into arg.  On timeout
 * (return 0) arg is untouched and the worker frees the copy exactly once,
 * whether the job never ran or finishes later.  The copied block must be
 * self-contained: any pointers stored inside it must target heap/persistent
 * storage, never the caller's stack. */
int Radio_RunOnNetWorkerCopied(void (*fn)(void *arg), void *arg, unsigned long arg_size);
int Radio_IsMemoryPoisoned(void);
void Radio_MarkMemoryPoisoned(const char *where);
int Radio_IsTlsPoisoned(void);
void Radio_MarkTlsPoisoned(const char *where);
/* Record a fatal-but-survivable TLS fault (SSL_ERROR_SSL from SSL_read):
 * the failing session's SSL objects are quarantined (leaked, never freed)
 * and HTTPS stays enabled -- always. Only detected memory corruption
 * hard-poisons HTTPS (Radio_MarkTlsPoisoned()). */
void Radio_SetTlsFaultContext(unsigned long session_id, const char *url);
void Radio_ReportTlsFault(const char *where);
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
static int Radio_WorkerIsIdle(void) { return 1; }
static const char *Radio_WorkerStateName(void) { return "idle"; }
static void Radio_GetAmiSslShared(void **amissl_base, void **amissl_ext_base, void **amissl_master_base)
{
    if (amissl_base) *amissl_base = 0;
    if (amissl_ext_base) *amissl_ext_base = 0;
    if (amissl_master_base) *amissl_master_base = 0;
}
static int Radio_AmiSslTaskIsOpener(void) { return 0; }
static int Radio_AmiSslLock(void) { return 0; }
static void Radio_AmiSslUnlock(void) { }
static int Radio_RunOnNetWorker(void (*fn)(void *arg), void *arg) { (void)fn; (void)arg; return 0; }
static int Radio_RunOnNetWorkerCopied(void (*fn)(void *arg), void *arg, unsigned long arg_size) { (void)fn; (void)arg; (void)arg_size; return 0; }
static int Radio_IsMemoryPoisoned(void) { return 0; }
static void Radio_MarkMemoryPoisoned(const char *where) { (void)where; }
static int Radio_IsTlsPoisoned(void) { return 0; }
static void Radio_MarkTlsPoisoned(const char *where) { (void)where; }
static void Radio_SetTlsFaultContext(unsigned long session_id, const char *url) { (void)session_id; (void)url; }
static void Radio_ReportTlsFault(const char *where) { (void)where; }
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

#endif /* RADIO_STREAM_H */

#if !defined(RADIO_DEBUG) && !defined(main) && !defined(RADIO_RELEASE_PRINTF_FILTER_DISABLED) && !defined(RADIO_RELEASE_PRINTF_FILTER_INSTALLED)
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
static int radio_release_printf(const char *fmt, ...)
{
    int r;
    va_list ap;

    if (fmt &&
        (!strncmp(fmt, "radio-runtime:", 14) ||
         !strncmp(fmt, "radio-probe: flag check", 23) ||
         !strncmp(fmt, "radio-art: flag check", 21) ||
         !strncmp(fmt, "radio-resource:", 15) ||
         !strncmp(fmt, "radio-read: transient zero", 26) ||
         !strncmp(fmt, "radio-input: zero read", 22) ||
         !strncmp(fmt, "radio-worker: session=", 22) ||
         !strncmp(fmt, "radio-worker: backpressure", 26) ||
         !strncmp(fmt, "radio-cleanup: abort SSL_free policy", 36) ||
         !strncmp(fmt, "radio-cleanup: abort SSL_free/SSL_CTX_free skipped", 51) ||
         !strncmp(fmt, "radio-tls-close:", 16) ||
         !strncmp(fmt, "radio-pump: stop/detach observed", 33)))
        return 0;

    va_start(ap, fmt);
    r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}
#define printf radio_release_printf
#define RADIO_RELEASE_PRINTF_FILTER_INSTALLED 1
#endif

#if defined(AMIGA_M68K) && defined(RB_GID_RADIO_RESULTS) && !defined(RADIO_GADTOOLS_CLOSE_GUARD_INSTALLED)
static int Radio_GadToolsIsRadioWindow(struct Window *win)
{
    return win && win->Title && strcmp((const char *)win->Title, "Internet Radio") == 0;
}

static int Radio_GadToolsGuardModifyIDCMP(struct Window *win, ULONG flags)
{
    return ModifyIDCMP(win, flags);
}

static UWORD Radio_GadToolsGuardRemoveGList(struct Window *win, struct Gadget *gadgets, WORD num)
{
    if (Radio_GadToolsIsRadioWindow(win)) {
        (void)gadgets;
        (void)num;
        return 0;
    }
    return RemoveGList(win, gadgets, num);
}

#define ModifyIDCMP(win, flags) Radio_GadToolsGuardModifyIDCMP((win), (flags))
#define RemoveGList(win, gadgets, num) Radio_GadToolsGuardRemoveGList((win), (gadgets), (num))
#define RADIO_GADTOOLS_CLOSE_GUARD_INSTALLED 1
#endif
