#ifndef ENABLE_RADIO
#define ENABLE_RADIO 0
#endif
#if ENABLE_RADIO
#include "radio_stream.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include "miniamp_memguard.h"

#ifndef RADIO_RING_BYTES
#define RADIO_RING_BYTES 65536UL
#endif
#ifndef RADIO_START_THRESHOLD
#define RADIO_START_THRESHOLD (RADIO_RING_BYTES / 4)
#endif
#ifndef RADIO_LOW_WATER_BYTES
#define RADIO_LOW_WATER_BYTES 4096UL
#endif
#ifndef RADIO_RECONNECT_MAX
#define RADIO_RECONNECT_MAX 10
#endif
#ifndef RADIO_RECONNECT_BACKOFF_PUMPS
#define RADIO_RECONNECT_BACKOFF_PUMPS 16
#endif
#define RADIO_HEADER_MAX 4096
#define RADIO_META_MAX 512
#ifndef RADIO_ZERO_BYTE_PUMP_MAX
#define RADIO_ZERO_BYTE_PUMP_MAX 64
#endif
#ifndef RADIO_START_TIMEOUT_PUMPS
#define RADIO_START_TIMEOUT_PUMPS 150
#endif
#if defined(RADIO_DEBUG) || defined(RADIO_DEBUG_OPEN)
#define RADIO_OPEN_DEBUG_PRINTF(x) RADIO_DBG_PRINTF(x)
#else
#define RADIO_OPEN_DEBUG_PRINTF(x) ((void)0)
#endif
#undef RADIO_STOP_DEBUG_PRINTF
#if defined(RADIO_DEBUG) || RADIO_DEBUG_STOP
#define RADIO_STOP_DEBUG_PRINTF(x) RADIO_DBG_PRINTF(x)
#define RADIO_CLEANUP_DEBUG_PRINTF(x) RADIO_DBG_PRINTF(x)
#else
#define RADIO_STOP_DEBUG_PRINTF(x) ((void)0)
#define RADIO_CLEANUP_DEBUG_PRINTF(x) ((void)0)
#endif


#if defined(AMIGA_M68K)
#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/bsdsocket.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
struct Library *SocketBase = NULL;
#define RADIO_SOCKET long
#define RADIO_INVALID_SOCKET (-1)
#define radio_close_socket(s) CloseSocket(s)
#if defined(HAVE_AMISSL)
#include <libraries/amisslmaster.h>
#include <proto/amisslmaster.h>
#include <proto/amissl.h>
#include <amissl/amissl.h>
#include <errno.h>
/* Strong definitions — probe and browser_http files use weak refs to these. */
struct Library *AmiSSLMasterBase = NULL;
struct Library *AmiSSLBase = NULL;
struct Library *AmiSSLExtBase = NULL;
static int radio_amissl_initialized = 0;
/* Set alongside RadioStream::sslStatePoisoned (see Radio_Pump()'s
 * SSL_ERROR_SSL handling) -- mirrors it at file scope so
 * radio_ssl_global_cleanup() can skip CleanupAmiSSL() too, without needing
 * an rs pointer at both of its call sites (Radio_Close() has one,
 * Radio_NetworkShutdown() at final app exit does not). One task only ever
 * runs one playback session, so a single flag is enough. */
static int radio_amissl_task_poisoned = 0;
#endif /* HAVE_AMISSL */
#else
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#define RADIO_SOCKET int
#define RADIO_INVALID_SOCKET (-1)
#define radio_close_socket(s) close(s)
#endif

/* Non-blocking ioctl request and "no data yet" errno values.  Define local
 * fallbacks rather than pulling in <sys/ioctl.h>/<sys/errno.h>, whose presence
 * varies across the m68k netinclude (and to avoid the header churn that broke
 * an earlier attempt).  0x8004667E is the standard BSD/bsdsocket FIONBIO. */

static void radio_format_ipv4_be(unsigned long addr_be, char *out, int out_size)
{
    unsigned char *b;

    if (!out || out_size <= 0) return;
    b = (unsigned char *)&addr_be;
    sprintf(out, "%u.%u.%u.%u",
            (unsigned int)b[0],
            (unsigned int)b[1],
            (unsigned int)b[2],
            (unsigned int)b[3]);
}

#ifndef FIONBIO
#define FIONBIO 0x8004667EUL
#endif
#ifndef EWOULDBLOCK
#define EWOULDBLOCK 35
#endif
#ifndef EAGAIN
#define EAGAIN 35
#endif
#ifndef EINPROGRESS
#define EINPROGRESS 36
#endif
#ifndef EISCONN
#define EISCONN 56
#endif

typedef enum {
    RADIO_PARSE_HEADER,
    RADIO_PARSE_AUDIO,
    RADIO_PARSE_META_LEN,
    RADIO_PARSE_META_PAYLOAD
} RadioParseState;

/* Transport close mode: GRACEFUL attempts SSL_shutdown() (only meaningful for
 * a clean stop of a healthy, still-connected TLS session); ABORT always skips
 * it.  Calling SSL_shutdown() on a session whose peer/socket is already in an
 * error, HTTP-error, or closed state is what produces the recoverable AmiSSL
 * alerts this mode split exists to avoid -- every failure/timeout/error path
 * must use ABORT. */
typedef enum {
    RADIO_CLOSE_GRACEFUL = 0,
    RADIO_CLOSE_ABORT = 1
} RadioCloseMode;

typedef enum {
    STREAM_ALLOCATED = 1u << 0,
    TRANSPORT_CONNECTED = 1u << 1,
    TLS_DONE = 1u << 2,
    HEADER_DONE = 1u << 3,
    DECODER_STARTED = 1u << 4,
    PLAYBACK_STARTED = 1u << 5
} RadioStreamStateFlag;
static const char *radio_close_mode_name(RadioCloseMode mode) { return mode == RADIO_CLOSE_GRACEFUL ? "graceful" : "abort"; }

#define RADIO_STREAM_MAGIC 0x52535452UL

struct RadioStream {
    unsigned long magic;
    RADIO_SOCKET sock;
    RadioStatus status;
    char url[256], host[128], path[192];
    int port, bitrate, metaint, audioUntilMeta, headerDone;
    unsigned int streamStateFlags;
    int decoderStarted, playbackStarted, audioInitialized;
    char contentType[64], title[128], stationName[128], genre[64], streamUrl[128], error[128];
    unsigned char *ring;
    unsigned char *ringAlloc;
    unsigned long ringLastWrite;
    unsigned long rpos, wpos, used, size;
    char header[RADIO_HEADER_MAX];
    int headerLen;
    RadioParseState parseState;
    unsigned char meta[RADIO_META_MAX];
    int metaLen, metaGot, metaLeft;
    int reconnectAttempts, reconnectDelay;
    int zeroBytePumps;
    int startPumps;
    int everPlayed;
    int firstDataLogged;
    int stopping;
    struct in_addr hostAddr;   /* cached DNS result so reconnects skip gethostbyname() */
    int haveHostAddr;
    int isSSL;
    unsigned long session_id;
    clock_t lastMemReportClock;
    unsigned int cleanup_count, stop_request_count, task_exit_count;
    unsigned int ssl_free_count, ssl_ctx_free_count, socket_close_count, decoder_free_count;
    unsigned int stream_buffer_free_count, audio_buffer_free_count, amissl_cleanup_count;
    unsigned int sslFreed, ctxFreed, socketClosed, cleanupDone;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    SSL *ssl;
    SSL_CTX *ctx;
    int sslHandshakeDone;
    int sslStatePoisoned; /* see radio_ssl_read()'s SSL_ERROR_SSL handling */
#endif
};

static unsigned long radio_next_session_id = 1;
static long radio_active_stream_sessions = 0;
static long radio_active_stream_tasks = 0;
static long radio_open_socket_count = 0;
static long radio_playback_open_socket_count = 0;
static long radio_active_ssl_count = 0;
static long radio_active_ssl_ctx_count = 0;
static long radio_active_decoder_count = 0;
static long radio_active_audio_buffer_count = 0;
static long radio_active_stream_buffer_count = 0;
static long radio_amissl_init_count = 0;
static long radio_active_icy_metadata_count = 0;
static long radio_gui_listbrowser_node_count = 0;
static long radio_gui_string_count = 0;
static int radio_atexit_registered = 0;
static int radioMemoryPoisoned = 0;
int radioAmiSslPoisoned = 0;
static unsigned long radio_poison_session_id = 0;
static char radio_poison_url[256];
/* First (root-cause) reason AmiSSL was marked poisoned this run; later
 * Radio_MarkTlsPoisoned() calls are consequences of the first and do not
 * overwrite it. */
static char radio_tls_poison_reason[96];

/* bsdsocket.library and AmiSSL are opened once by Radio_NetworkInit() at app
 * startup and closed once by Radio_NetworkShutdown() at app exit -- these
 * count those two events specifically (expected to reach 1 each over an
 * entire run), as opposed to the per-connection socket/SSL/SSL_CTX counts
 * above which are expected to churn constantly. */
static long radio_socket_library_open_count = 0;
static long radio_socket_library_close_count = 0;
static long radio_amissl_cleanup_count = 0;
static long radio_amisslmaster_open_count = 0;
static long radio_amisslmaster_close_count = 0;
static long radio_openamissltags_count = 0;
static long radio_closeamissl_count = 0;
/* Radio_NetworkShutdown() must be idempotent: the app-close path has a
 * fallback call site, and running the AmiSSL/bsdsocket teardown twice is
 * exactly the kind of double-cleanup this file's counters exist to catch. */
static int radio_network_shutdown_started = 0;

static void radio_debug_mem_report(unsigned long session_id, const char *where)
{
#if defined(AMIGA_M68K)
    RADIO_DBG(printf("radio-mem: session=%lu %s AvailMem(any)=%lu fast=%lu chip=%lu\n",
        session_id, where ? where : "", (unsigned long)AvailMem(MEMF_ANY),
        (unsigned long)AvailMem(MEMF_FAST), (unsigned long)AvailMem(MEMF_CHIP)));
#else
    RADIO_DBG(printf("radio-mem: session=%lu %s AvailMem(any)=n/a fast=n/a chip=n/a\n",
        session_id, where ? where : ""));
#endif
}

void Radio_MarkMemoryPoisoned(const char *where)
{
    radioMemoryPoisoned = 1;
    printf("radio-memory: Memory corruption detected - restart app (where=%s session=%lu url=\"%s\")\n",
        where ? where : "", radio_poison_session_id,
        radio_poison_url[0] ? radio_poison_url : "");
}

int Radio_IsMemoryPoisoned(void)
{
    return radioMemoryPoisoned;
}

const char *Radio_TlsPoisonedMessage(void)
{
    /* Reboot, not just app restart: once poisoned cleanup has been skipped,
     * the corrupted amissl.library stays resident with a nonzero open count
     * (AmigaOS never reclaims library opens from an exited process), so a
     * relaunch gets handed the same broken library. */
    return "HTTPS disabled after fatal TLS/memory faults; reboot before using HTTPS.";
}

/* Fatal SSL_read faults are now survivable: the session fails cleanly and
 * HTTPS stays enabled (see Radio_ReportTlsFault()). Only repeated faults in
 * one run -- corruption-grade misbehaviour, not one bad station -- blow this
 * fuse and hard-poison HTTPS. */
#define RADIO_TLS_FAULT_LIMIT 6
static long radio_tls_fault_count = 0;

void Radio_ReportTlsFault(const char *where)
{
    radio_tls_fault_count++;
    RADIO_DBG(printf("radio-tls: TLS fault %ld of %d where=%s session=%lu url=\"%s\" -- HTTPS stays enabled below the limit\n",
        radio_tls_fault_count, RADIO_TLS_FAULT_LIMIT, where ? where : "",
        radio_poison_session_id, radio_poison_url[0] ? radio_poison_url : ""));
    if (radio_tls_fault_count >= RADIO_TLS_FAULT_LIMIT)
        Radio_MarkTlsPoisoned(where);
}

void Radio_MarkTlsPoisoned(const char *where)
{
    if (!radioAmiSslPoisoned) {
        printf("%s\n", Radio_TlsPoisonedMessage());
        if (where && where[0]) {
            strncpy(radio_tls_poison_reason, where, sizeof(radio_tls_poison_reason) - 1);
            radio_tls_poison_reason[sizeof(radio_tls_poison_reason) - 1] = '\0';
        }
    }
    radioAmiSslPoisoned = 1;
    RADIO_DBG(printf("radio-tls: AmiSSL poisoned where=%s session=%lu url=\"%s\"\n",
        where ? where : "", radio_poison_session_id,
        radio_poison_url[0] ? radio_poison_url : ""));
}

int Radio_IsTlsPoisoned(void)
{
    return radioAmiSslPoisoned;
}

const char *Radio_TlsPoisonReason(void)
{
    return radio_tls_poison_reason[0] ? radio_tls_poison_reason : "not-poisoned";
}

int Radio_CheckMiniMem(const char *where)
{
    int corrupt = MiniMem_CheckAll(where);
    if (corrupt > 0)
        Radio_MarkMemoryPoisoned(where);
    return corrupt;
}

static int radio_stream_magic_valid(const RadioStream *rs, const char *where)
{
    int ok = rs && rs->magic == RADIO_STREAM_MAGIC;
    if (!ok) RADIO_DBG(printf("radio-guard: BAD RadioStream magic where=%s rs=%p magic=%08lx expected=%08lx\n", where ? where : "", (void *)rs, rs ? rs->magic : 0UL, RADIO_STREAM_MAGIC););
    return ok;
}

static void radio_resource_summary(const RadioStream *rs, const char *where)
{
    RADIO_DBG(printf("radio-summary: session=%lu %s active_stream_sessions=%ld active_stream_tasks=%ld open_socket_count=%ld playback_open_socket_count=%ld active_ssl_count=%ld active_ssl_ctx_count=%ld active_decoder_count=%ld active_audio_buffer_count=%ld active_stream_buffer_count=%ld active_icy_metadata_count=%ld active_gui_nodes=%ld active_gui_strings=%ld amissl_init_count=%ld cleanup_count=%u\n",
        rs ? rs->session_id : 0, where ? where : "", radio_active_stream_sessions,
        radio_active_stream_tasks, radio_open_socket_count, radio_playback_open_socket_count, radio_active_ssl_count,
        radio_active_ssl_ctx_count, radio_active_decoder_count,
        radio_active_audio_buffer_count, radio_active_stream_buffer_count,
        radio_active_icy_metadata_count, radio_gui_listbrowser_node_count,
        radio_gui_string_count, radio_amissl_init_count, rs ? rs->cleanup_count : 0));
}
static void radio_app_exit_report(void)
{
    radio_debug_mem_report(0, "before app close");
    radio_resource_summary(NULL, "before app close");
}

static int radio_is_stopping(const RadioStream *rs) { return !rs || rs->stopping || rs->status == RADIO_STATUS_STOPPING || rs->status == RADIO_STATUS_CLOSED; }
static void close_current_socket(RadioStream *rs);
static int radio_contains_nocase(const char *s, const char *needle)
{
    int n, i;
    if (!s || !needle) return 0;
    n = (int)strlen(needle);
    if (n <= 0) return 1;
    for (i = 0; s[i]; i++) {
        int j;
        for (j = 0; j < n && s[i + j] && tolower((unsigned char)s[i + j]) == tolower((unsigned char)needle[j]); j++) ;
        if (j == n) return 1;
    }
    return 0;
}
static int radio_url_looks_hls(const char *url) { return radio_contains_nocase(url, ".m3u8"); }
static void radio_duplicate_cleanup_warning(RadioStream *rs, const char *what, unsigned int count)
{
    if (rs && count > 1)
        RADIO_DBG(printf("radio-cleanup warning: session=%lu duplicate %s count=%u; skipping duplicate operation\n", rs->session_id, what, count););
}
static void radio_reset_session_state(RadioStream *rs)
{
    if (!rs) return;
    rs->sock = RADIO_INVALID_SOCKET;
    rs->isSSL = 0;
    rs->lastMemReportClock = 0;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    rs->ssl = NULL;
    rs->ctx = NULL;
    rs->sslHandshakeDone = 0;
#endif
    rs->bitrate = rs->metaint = rs->audioUntilMeta = rs->headerDone = 0;
    rs->streamStateFlags = 0;
    rs->decoderStarted = rs->playbackStarted = rs->audioInitialized = 0;
    rs->contentType[0] = rs->title[0] = rs->stationName[0] = rs->genre[0] = rs->streamUrl[0] = rs->error[0] = 0;
    rs->rpos = rs->wpos = rs->used = 0;
    rs->headerLen = 0; rs->header[0] = 0;
    rs->parseState = RADIO_PARSE_HEADER;
    rs->metaLen = rs->metaGot = rs->metaLeft = 0;
    rs->reconnectAttempts = rs->reconnectDelay = rs->zeroBytePumps = rs->startPumps = 0;
    rs->everPlayed = rs->firstDataLogged = rs->haveHostAddr = 0;
    rs->sslFreed = rs->ctxFreed = rs->socketClosed = rs->cleanupDone = 0;
}


/* Yield the CPU briefly during reconnect backoff.  reconnect_http() is the only
 * pump path that does not block on the socket, so without this the player
 * process spins at 100% CPU while the stream is down or re-buffering, starving
 * Workbench and the GUI's window redraws (the "desktop/mouse locks up while the
 * internet buffers" symptom). */
static void radio_backoff_sleep(void)
{
#if defined(AMIGA_M68K)
    Delay(2); /* ~40ms (2 ticks @ 50Hz) */
#else
    usleep(40000);
#endif
}

/* Put the stream socket into non-blocking mode.  WinUAE's built-in
 * bsdsocket.library runs a *blocking* socket call synchronously and freezes the
 * whole emulation (mouse included) until it returns - so a blocking recv() that
 * waits for the ring to refill freezes the machine for ~1s every time the
 * stream re-buffers.  With a non-blocking socket recv() returns immediately and
 * we yield with Delay() (a pure timer wait the emulator does not stall on). */
static void radio_set_nonblocking(RADIO_SOCKET s)
{
#if defined(AMIGA_M68K)
    long nb = 1;
    IoctlSocket(s, FIONBIO, (char *)&nb);
#else
    int fl = fcntl(s, F_GETFL, 0);
    if (fl >= 0)
        fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
}

/* True when the last socket call failed only because no data is ready yet. */
static int radio_would_block(void)
{
#if defined(AMIGA_M68K)
    long e = Errno();
    return e == EWOULDBLOCK || e == EAGAIN;
#else
    return errno == EWOULDBLOCK || errno == EAGAIN;
#endif
}

static long radio_sock_errno(void)
{
#if defined(AMIGA_M68K)
    return Errno();
#else
    return errno;
#endif
}

static long radio_ioerr_value(void)
{
#if defined(AMIGA_M68K)
    return IoErr();
#else
    return 0;
#endif
}

static void radio_log_socket_failure(RadioStream *rs, const char *context, const char *where)
{
    RADIO_DBG(printf("radio-socket: %s socket failed session=%lu host=%s fd=-1 errno=%ld IoErr=%ld open_socket_count=%ld playback_open_socket_count=%ld active_stream_sessions=%ld active_stream_tasks=%ld active_ssl_count=%ld active_ssl_ctx_count=%ld current_state=%d previous_stream_state=n/a old_child_done_posted=n/a parent_done_received=n/a where=%s\n",
        context ? context : "playback", rs ? rs->session_id : 0, rs ? rs->host : "",
        radio_sock_errno(), radio_ioerr_value(), radio_open_socket_count,
        radio_playback_open_socket_count, radio_active_stream_sessions,
        radio_active_stream_tasks, radio_active_ssl_count, radio_active_ssl_ctx_count,
        rs ? (int)rs->status : -1, where ? where : ""));
}

/* Forward declaration so the SSL helpers can call set_error before it is
 * defined later in this translation unit. */
static void set_error(RadioStream *rs, const char *msg);

#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
/* Open amisslmaster.library and the shared AmiSSL instance (AmiSSLBase/
 * AmiSSLExtBase via OpenAmiSSLTags()) exactly once for the whole app run,
 * per the AmiSSL v5 model: the main process opens the shared instance and
 * every task then only runs its own InitAmiSSL()/CleanupAmiSSL() pair
 * against it -- amissl.library itself is never OpenLibrary()'d directly or
 * repeatedly. Normally called from Radio_NetworkInit() in the main task at
 * app startup; radio_ssl_global_init()/rb_probe_ensure_amissl() keep a lazy
 * fallback for builds that skip Radio_NetworkInit(). Requires SocketBase.
 * Returns 0 when the shared instance is (already) open. */
static int radio_amissl_open_shared(void)
{
    if (AmiSSLBase) return 0;
    if (!SocketBase) return -1;
    if (!AmiSSLMasterBase) {
        AmiSSLMasterBase = OpenLibrary("amisslmaster.library", AMISSLMASTER_MIN_VERSION);
        if (!AmiSSLMasterBase) return -1;
        radio_amisslmaster_open_count++;
        RADIO_DBG(printf("radio-netinit: amisslmaster.library opened amisslmaster_open_count=%ld\n", radio_amisslmaster_open_count););
        if (!InitAmiSSLMaster(AMISSL_CURRENT_VERSION, TRUE)) {
            CloseLibrary(AmiSSLMasterBase); AmiSSLMasterBase = NULL;
            radio_amisslmaster_close_count++;
            return -1;
        }
    }
    if (OpenAmiSSLTags(AMISSL_CURRENT_VERSION,
                       AmiSSL_UsesOpenSSLStructs, TRUE,
                       AmiSSL_GetAmiSSLBase, (ULONG)&AmiSSLBase,
                       AmiSSL_GetAmiSSLExtBase, (ULONG)&AmiSSLExtBase,
                       AmiSSL_SocketBase, (ULONG)SocketBase,
                       AmiSSL_ErrNoPtr, (ULONG)&errno,
                       TAG_DONE) != 0) {
        return -1;
    }
    radio_openamissltags_count++;
    RADIO_DBG(printf("radio-netinit: OpenAmiSSLTags OK base=%p ext=%p openamissltags_count=%ld\n", (void *)AmiSSLBase, (void *)AmiSSLExtBase, radio_openamissltags_count););
    return 0;
}

static int radio_ssl_global_init(RadioStream *rs)
{
    if (Radio_IsTlsPoisoned()) {
        /* AmiSSL cleanup has already been skipped/leaked for this run --
         * never call back into it. */
        set_error(rs, Radio_TlsPoisonedMessage());
        return -1;
    }
    if (!SocketBase) {
        SocketBase = OpenLibrary("bsdsocket.library", 4);
        if (!SocketBase) { set_error(rs, "AmiSSL unavailable: bsdsocket.library unavailable"); return -1; }
        radio_socket_library_open_count++;
        RADIO_DBG(printf("radio-netinit: bsdsocket.library opened socket_library_open_count=%ld\n", radio_socket_library_open_count););
    }
    /* radio_amissl_initialized (this file's own flag, NOT AmiSSLBase) is
     * "have I, this task, called InitAmiSSL() yet" -- AmiSSLBase now stays
     * open for the whole app's lifetime (see Radio_NetworkInit()/
     * Radio_NetworkShutdown()) rather than being opened and closed on every
     * station switch, per amissl.library's own recommendation ("share the
     * AmiSSL instance opened from your main process and have each
     * [subprocess] simply call InitAmiSSLA() instead of ... opening AmiSSL
     * for every process"). Checking AmiSSLBase itself here would be wrong
     * now: it stays non-NULL across every task, so it can no longer answer
     * "has *this* task called InitAmiSSL()" the way it used to when the
     * library was opened and closed per session.
     *
     * Each task must still call its own InitAmiSSL(), paired with its own
     * CleanupAmiSSL() -- that part of the per-task contract is unchanged and
     * still required; only the library-level open/close moved to app
     * startup/shutdown. Reusing another task's InitAmiSSL() result without
     * calling InitAmiSSL() again in this task is what crashes inside
     * SSL_CTX_new()/SSL_new(). */
    if (radio_amissl_initialized) return 0;
    if (!AmiSSLBase) {
        /* Shared instance not open yet (Radio_NetworkInit() didn't get to
         * it, or this is a build that calls straight into
         * radio_ssl_global_init() without it) -- lazily open it here.
         * Whoever gets here first opens it for the rest of the app's
         * lifetime. */
        if (radio_amissl_open_shared() != 0) {
            set_error(rs, "AmiSSL unavailable"); return -1;
        }
    }
    /* Deliberately using AmiSSL's own auto-allocated timer.device port here
     * (not AmiSSL_TimerPort with one of our own) -- a caller-supplied port
     * was tried and reverted after it correlated with the playback child
     * going completely unresponsive to StopPlayback()'s SIGBREAKF_CTRL_C
     * while mid-SSL_connect() (WANT_READ), timing out the parent instead of
     * the usual clean abort. Whatever AmiSSL's default timer-port wiring
     * does differently from a bare CreateMsgPort() one, it's required for
     * SSL_connect()'s own retry/timeout handling to keep noticing external
     * signals -- so leave it alone. */
    if (InitAmiSSL(AmiSSL_SocketBase, (ULONG)SocketBase,
                   AmiSSL_ErrNoPtr, (ULONG)&errno,
                   TAG_DONE) != 0) {
        /* Do NOT CloseAmiSSL() here: this task may not be the one that
         * opened the shared library, and other tasks (or a later retry)
         * may still need it -- only Radio_NetworkShutdown() closes it. */
        set_error(rs, "AmiSSL init failed"); return -1;
    }
    radio_amissl_initialized = 1;
    radio_amissl_init_count++;
    RADIO_DBG(printf("radio-resource: session=%lu AmiSSL init count=%ld\n", rs ? rs->session_id : 0, radio_amissl_init_count););
    return 0;
}

static void radio_ssl_global_cleanup(void)
{
    RADIO_DBG(printf("radio-ssl-diag: cleanup ENTER initialized=%d base=%p ext=%p master=%p\n", radio_amissl_initialized, (void *)AmiSSLBase, (void *)AmiSSLExtBase, (void *)AmiSSLMasterBase););
    RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: AmiSSL global cleanup start initialized=%d base=%p master=%p\n", radio_amissl_initialized, (void *)AmiSSLBase, (void *)AmiSSLMasterBase));
    if (radio_amissl_initialized && (radio_amissl_task_poisoned || Radio_IsTlsPoisoned())) {
        /* The soak test that found the SSL_ERROR_SSL/ring-corruption chain
         * also caught the *next* crash one step further down, right inside
         * CleanupAmiSSL() itself, once SSL_free()/SSL_CTX_free() were
         * already being skipped for this poisoned task -- confirming
         * AmiSSL's corruption here isn't confined to the SSL/SSL_CTX
         * objects. Skip this call too and just leave this task's per-task
         * AmiSSL init state unreleased rather than crash on top of it.
         * The check covers the app-wide flag as well as this task's own:
         * once any task has poisoned AmiSSL, its internals are suspect for
         * every task, so nobody gets to call CleanupAmiSSL() again. */
        RADIO_DBG(printf("radio-cleanup: CleanupAmiSSL skipped (poisoned) leaking per-task AmiSSL state to avoid crashing on corrupted AmiSSL internals\n"););
        Radio_MarkTlsPoisoned("CleanupAmiSSL skipped for poisoned AmiSSL session");
        radio_amissl_initialized = 0;
    } else if (radio_amissl_initialized) {
        RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: CleanupAmiSSL start\n"));
        CleanupAmiSSL(TAG_DONE);
        radio_amissl_initialized = 0;
        radio_amissl_cleanup_count++;
        RADIO_DBG(printf("radio-netshutdown: AmiSSL cleaned up amissl_cleanup_count=%ld\n", radio_amissl_cleanup_count););
        RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: CleanupAmiSSL done\n"));
    } else {
        RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: CleanupAmiSSL skipped\n"));
    }
    radio_amissl_task_poisoned = 0;
    /* Keep the shared AmiSSL instance (AmiSSLBase/AmiSSLExtBase) *and*
     * amisslmaster.library open for the lifetime of the program now --
     * CloseAmiSSL() is called exactly once, from Radio_NetworkShutdown(),
     * not after every session. AmiSSL requires InitAmiSSLMaster() to run
     * exactly once; only the per-task InitAmiSSL()/CleanupAmiSSL() pair
     * above is allowed to repeat. The previous code closed and re-opened
     * the master library on every stream stop, so a stop->probe->start
     * cycle (Play pressed on an already-playing stream) re-ran
     * InitAmiSSLMaster() several times in quick succession and wedged the
     * next HTTPS connection, freezing the machine.  AmiSSLBase/
     * AmiSSLMasterBase are shared with radio_stream_probe.c through their
     * weak symbols and the OS reclaims them at program exit regardless. */
    RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: AmiSSL base kept open base=%p master=%p\n", (void *)AmiSSLBase, (void *)AmiSSLMasterBase));
    RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: AmiSSL global cleanup complete\n"));
    RADIO_DBG(printf("radio-ssl-diag: cleanup EXIT  initialized=%d base=%p ext=%p master=%p\n", radio_amissl_initialized, (void *)AmiSSLBase, (void *)AmiSSLExtBase, (void *)AmiSSLMasterBase););
}

static void radio_ssl_close_stream_mode(RadioStream *rs, RadioCloseMode mode);
static void radio_ssl_close_stream(RadioStream *rs);
static void radio_abort_current_socket(RadioStream *rs);
static void close_current_socket(RadioStream *rs);

/* Poll SSL_connect on the non-blocking socket — same budget as radio_wait_connected. */
static int radio_ssl_do_handshake(RadioStream *rs)
{
    int tries;
    int last_error = 0;
    /* Start with a clean OpenSSL error queue: a stale entry left by an
     * earlier failed connection would otherwise be misread as this
     * connection's fatal error by the fault handling below. */
    ERR_clear_error();
    for (tries = 0; tries < 150; tries++) {
        int r, e;
        if (radio_is_stopping(rs)) return -1;
        r = SSL_connect(rs->ssl);
        if (r == 1) {
            RADIO_DBG(printf("SSL_CONNECT_ATTEMPT session=%lu attempt=%d ret=%d err=0 fd=%ld ssl=%p ctx=%p handshake=%d\n", rs ? rs->session_id : 0, tries + 1, r, rs ? (long)rs->sock : -1L, rs ? (void *)rs->ssl : 0, rs ? (void *)rs->ctx : 0, rs ? rs->sslHandshakeDone : 0););
            if (rs) rs->sslHandshakeDone = 1;
            RADIO_DBG(printf("SSL_CONNECT_DONE session=%lu\n", rs ? rs->session_id : 0););
            return 0;
        }
        e = SSL_get_error(rs->ssl, r);
        last_error = e;
        RADIO_DBG(printf("SSL_CONNECT_ATTEMPT session=%lu attempt=%d ret=%d err=%d fd=%ld ssl=%p ctx=%p handshake=%d\n", rs ? rs->session_id : 0, tries + 1, r, e, rs ? (long)rs->sock : -1L, rs ? (void *)rs->ssl : 0, rs ? (void *)rs->ctx : 0, rs ? rs->sslHandshakeDone : 0););
        if (e == SSL_ERROR_WANT_READ) {
            RADIO_DBG(printf("SSL_CONNECT_WANT_READ session=%lu attempt=%d retry\n", rs ? rs->session_id : 0, tries + 1););
            radio_backoff_sleep(); continue;
        }
        if (e == SSL_ERROR_WANT_WRITE) {
            RADIO_DBG(printf("SSL_CONNECT_WANT_WRITE session=%lu attempt=%d retry\n", rs ? rs->session_id : 0, tries + 1););
            radio_backoff_sleep(); continue;
        }
        /* SSL_ERROR_WANT_READ/WRITE just means "call again later" on a
         * non-blocking socket; anything else is a real handshake failure,
         * and the OpenSSL/AmiSSL error queue has the actual reason (cipher
         * mismatch, protocol version, rejected cert, ...) that
         * SSL_get_error()'s numeric code alone doesn't say. */
        {
            unsigned long ssl_lib_error = ERR_get_error();
            char ssl_error_buf[160];
            ssl_error_buf[0] = '\0';
            if (ssl_lib_error != 0)
                ERR_error_string_n(ssl_lib_error, ssl_error_buf, sizeof(ssl_error_buf));
            RADIO_DBG(printf("SSL_CONNECT_FATAL session=%lu err=%d lib_error=%08lx (%s)\n",
                rs ? rs->session_id : 0, e, ssl_lib_error, ssl_error_buf[0] ? ssl_error_buf : "none"););
            /* Handshake failures are a normal per-server outcome (protocol/
             * cipher mismatch, bad record); drain the rest of the queue so
             * the failure cannot masquerade as a later session's fault. */
            ERR_clear_error();
        }
        return -1;
    }
    RADIO_DBG(printf("radio-tls: SSL_connect timeout session=%lu last_ssl_error=%d fd=%ld\n", rs ? rs->session_id : 0, last_error, rs ? (long)rs->sock : -1L););
    return -1;
}

static int radio_ssl_connect(RadioStream *rs)
{
    const SSL_METHOD *method;
    int set_fd_ok;
    if (radio_ssl_global_init(rs) != 0) return -1;
    /* rs->ctx persists across reconnects within this task (see
     * radio_ssl_free_ctx(), only called once from Radio_Close()) -- a
     * dropped stream can reconnect up to RADIO_RECONNECT_MAX (10) times,
     * and rebuilding a whole new SSL_CTX (with its own cert-verify setup)
     * on every one of those, on top of a fresh SSL_CTX per station switch,
     * multiplies AmiSSL's create/destroy cycle count well beyond what's
     * actually needed -- only a fresh SSL object is required per connection
     * attempt, per normal OpenSSL practice. */
    if (rs->ctx && rs->sslStatePoisoned) {
        /* This ctx survived a prior SSL_ERROR_SSL on this same reconnecting
         * session (see Radio_Pump()) and was deliberately leaked rather
         * than freed, since AmiSSL's own internal state may already be
         * corrupted. Reusing it for a fresh SSL_new() here would be exactly
         * the "call back into corrupted AmiSSL state" this app is trying to
         * avoid -- orphan it and force a brand new SSL_CTX for the retry
         * instead, same as if this were the first connection attempt. */
        RADIO_DBG(printf("radio-resource: session=%lu orphaning poisoned SSL_CTX %p instead of reusing it for reconnect\n", rs->session_id, (void *)rs->ctx));
        if (radio_active_ssl_ctx_count > 0) radio_active_ssl_ctx_count--;
        rs->ctx = NULL;
        rs->ctxFreed = 1;
        rs->sslStatePoisoned = 0;
    }
    if (!rs->ctx) {
        method = SSLv23_client_method();
        if (!method) { set_error(rs, "AmiSSL init failed"); return -1; }
        rs->ctx = SSL_CTX_new(method);
        if (rs->ctx) { rs->ctxFreed = 0; radio_active_ssl_ctx_count++; RADIO_DBG(printf("radio-resource: session=%lu SSL_CTX allocated active_ssl_ctx_count=%ld\n", rs->session_id, radio_active_ssl_ctx_count)); }
        if (!rs->ctx) { set_error(rs, "AmiSSL init failed"); return -1; }
#ifdef RADIO_SSL_VERIFY_PEER
        /* SSLCERTS=1 build: verify the server's certificate chain against
         * AmiSSL's installed root CA bundle -- SSL_CTX_set_default_verify_paths()
         * picks it up the same way it would on any other OpenSSL-based build --
         * and the hostname against the certificate, instead of accepting
         * anything.  Falls back to no verification if the cert store can't be
         * loaded at all (e.g. AmiSSL's update pack with the root certs was
         * never installed), rather than refusing to play: a missing/broken
         * cert store shouldn't be able to break streaming outright. */
        if (SSL_CTX_set_default_verify_paths(rs->ctx)) {
            SSL_CTX_set_verify(rs->ctx, SSL_VERIFY_PEER, NULL);
            RADIO_DBG(printf("radio-tls: cert verification enabled session=%lu host=%s\n", rs->session_id, rs->host););
        } else {
            SSL_CTX_set_verify(rs->ctx, SSL_VERIFY_NONE, NULL);
            RADIO_DBG(printf("radio-tls: cert store unavailable, verification disabled session=%lu\n", rs->session_id););
        }
#else
        /* No CA bundle path assumed by default; skip cert verification for
         * streams.  Build with SSLCERTS=1 to verify against AmiSSL's
         * installed root certs instead (see above). */
        SSL_CTX_set_verify(rs->ctx, SSL_VERIFY_NONE, NULL);
#endif
    }
    rs->ssl = SSL_new(rs->ctx);
    if (rs->ssl) { rs->sslFreed = 0; radio_active_ssl_count++; RADIO_DBG(printf("radio-resource: session=%lu SSL allocated active_ssl_count=%ld\n", rs->session_id, radio_active_ssl_count)); }
    if (!rs->ssl) { radio_ssl_close_stream(rs); set_error(rs, "AmiSSL init failed"); return -1; }
#ifdef RADIO_SSL_VERIFY_PEER
    /* SNI and hostname verification only matter once we're actually
     * checking the chain; sending SNI unconditionally would be a (probably
     * harmless) behavior change to the default no-verify build that wasn't
     * asked for here. */
#ifdef SSL_CTRL_SET_TLSEXT_HOSTNAME
    SSL_set_tlsext_host_name(rs->ssl, rs->host);
#endif
    {
        X509_VERIFY_PARAM *verify_param = SSL_get0_param(rs->ssl);
        if (verify_param) X509_VERIFY_PARAM_set1_host(verify_param, rs->host, 0);
    }
#endif
    set_fd_ok = SSL_set_fd(rs->ssl, (int)rs->sock);
    if (!set_fd_ok) { RADIO_DBG(printf("radio-tls: SSL_set_fd failed session=%lu fd=%ld ssl=%p ctx=%p\n", rs->session_id, (long)rs->sock, (void *)rs->ssl, (void *)rs->ctx);); radio_ssl_close_stream(rs); set_error(rs, "TLS handshake failed"); return -1; }
    if (radio_ssl_do_handshake(rs) != 0) {
        RADIO_DBG(printf("radio-tls: handshake cleanup session=%lu error_ptr=%p stream_range=%p..%p status=%d open_socket_count=%ld active_ssl_count=%ld active_ssl_ctx_count=%ld SocketBase=%p AmiSSLBase=%p AmiSSLMasterBase=%p\n", rs->session_id, (void *)rs->error, (void *)rs, (void *)(rs + 1), (int)rs->status, radio_open_socket_count, radio_active_ssl_count, radio_active_ssl_ctx_count, (void *)SocketBase, (void *)AmiSSLBase, (void *)AmiSSLMasterBase););
        radio_ssl_close_stream(rs);
        set_error(rs, "TLS handshake failed"); return -1;
    }
    return 0;
}

static void radio_ssl_close_stream_mode(RadioStream *rs, RadioCloseMode mode)
{
    int shutdown_called = 0;
    if (!rs) return;
    RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: HTTPS cleanup start mode=%s ssl=%p ctx=%p fd=%ld handshake=%d\n", radio_close_mode_name(mode), (void *)rs->ssl, (void *)rs->ctx, (long)rs->sock, rs->sslHandshakeDone));
    if (mode == RADIO_CLOSE_GRACEFUL && !rs->sslStatePoisoned && !Radio_IsTlsPoisoned() && rs->ssl && rs->sslHandshakeDone && rs->sock != RADIO_INVALID_SOCKET && !rs->socketClosed) {
        RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: SSL_shutdown start ssl=%p\n", (void *)rs->ssl));
        SSL_shutdown(rs->ssl);
        shutdown_called = 1;
        RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: SSL_shutdown done\n"));
    } else {
        RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: SSL_shutdown skipped mode=%s ssl=%p fd=%ld handshake=%d\n", radio_close_mode_name(mode), (void *)rs->ssl, (long)rs->sock, rs->sslHandshakeDone));
    }
    RADIO_DBG(printf("radio-cleanup: ssl-close mode=%s session=%lu status=%d sslHandshakeDone=%d ssl_shutdown=%s ssl=%p ctx=%p fd=%ld open_socket_count=%ld\n",
        radio_close_mode_name(mode), rs->session_id, (int)rs->status, rs->sslHandshakeDone,
        shutdown_called ? "called" : "skipped", (void *)rs->ssl, (void *)rs->ctx, (long)rs->sock, radio_open_socket_count););
    if (rs->ssl && !rs->sslFreed && (rs->sslStatePoisoned || Radio_IsTlsPoisoned())) {
        /* See the SSL_ERROR_SSL handling in Radio_Pump(): AmiSSL's own
         * internal state may already be corrupted, so calling back into it
         * here risks crashing on top of that instead of just leaking one
         * SSL object. The app-wide flag counts too: another task's poison
         * means the same shared AmiSSL internals are suspect here. */
        RADIO_DBG(printf("radio-cleanup: SSL_free skipped (poisoned) session=%lu ssl=%p leaking to avoid crashing on corrupted AmiSSL state\n", rs->session_id, (void *)rs->ssl));
        Radio_MarkTlsPoisoned("SSL_free skipped for poisoned AmiSSL session");
        rs->sslFreed = 1;
        if (radio_active_ssl_count > 0) radio_active_ssl_count--;
        rs->ssl = NULL;
        rs->sslHandshakeDone = 0;
    } else if (rs->ssl && !rs->sslFreed) {
        RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: SSL_free start session=%lu ssl=%p ctx=%p fd=%ld\n", rs->session_id, (void *)rs->ssl, (void *)rs->ctx, (long)rs->sock));
        radio_debug_mem_report(rs->session_id, "before SSL_free");
        rs->ssl_free_count++;
        SSL_free(rs->ssl);
        rs->sslFreed = 1;
        if (radio_active_ssl_count > 0) radio_active_ssl_count--;
        rs->ssl = NULL;
        rs->sslHandshakeDone = 0;
        radio_debug_mem_report(rs->session_id, "after SSL_free");
        RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: SSL_free done\n"));
    } else {
        RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: SSL_free skipped\n"));
    }
    /* rs->ctx is deliberately NOT freed here -- it's reused across
     * reconnects within this task (see radio_ssl_connect()). It's only
     * freed once, by radio_ssl_free_ctx(), when this session is exiting for
     * good (Radio_Close()). */
    RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: HTTPS cleanup complete\n"));
}

/* Safe default used by every error/timeout/handshake-failure path: never
 * attempts SSL_shutdown() on a session that may already be in an error or
 * peer-closed state. */
static void radio_ssl_close_stream(RadioStream *rs) { radio_ssl_close_stream_mode(rs, RADIO_CLOSE_ABORT); }

/* Final, once-per-task free of the SSL_CTX kept alive across this session's
 * reconnects (see radio_ssl_connect()). Called once from Radio_Close(). */
static void radio_ssl_free_ctx(RadioStream *rs)
{
    if (!rs) return;
    if (rs->ctx && !rs->ctxFreed && (rs->sslStatePoisoned || Radio_IsTlsPoisoned())) {
        /* See the SSL_ERROR_SSL handling in Radio_Pump(): the crash this
         * mitigates was caught by the soak test landing right inside this
         * exact call, immediately after a ring-buffer corruption was found
         * that traces back to the same poisoned session -- so SSL_CTX_free()
         * itself, not just SSL_free(), needs to be skipped once poisoned
         * (again including app-wide poison from another task). */
        RADIO_DBG(printf("radio-cleanup: SSL_CTX_free skipped (poisoned) session=%lu ctx=%p leaking to avoid crashing on corrupted AmiSSL state\n", rs->session_id, (void *)rs->ctx));
        Radio_MarkTlsPoisoned("SSL_CTX_free skipped for poisoned AmiSSL session");
        rs->ctxFreed = 1;
        if (radio_active_ssl_ctx_count > 0) radio_active_ssl_ctx_count--;
        rs->ctx = NULL;
    } else if (rs->ctx && !rs->ctxFreed) {
        RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: SSL_CTX_free start session=%lu ctx=%p fd=%ld\n", rs->session_id, (void *)rs->ctx, (long)rs->sock));
        radio_debug_mem_report(rs->session_id, "before SSL_CTX_free");
        rs->ssl_ctx_free_count++;
        SSL_CTX_free(rs->ctx);
        rs->ctxFreed = 1;
        if (radio_active_ssl_ctx_count > 0) radio_active_ssl_ctx_count--;
        rs->ctx = NULL;
        radio_debug_mem_report(rs->session_id, "after SSL_CTX_free");
        RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: SSL_CTX_free done\n"));
    } else {
        RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: SSL_CTX_free skipped\n"));
    }
}
#endif /* AMIGA_M68K && HAVE_AMISSL */

/* Drive a non-blocking connect() to completion by re-issuing connect() and
 * yielding with Delay() between tries, so the connect never blocks (and so
 * never freezes WinUAE's emulation).  Returns 0 on success, -1 on failure or
 * stop/timeout.  Success is connect()==0 or EISCONN; anything else is treated
 * as "still connecting" until the timeout, which keeps us robust even if a
 * particular stack reports a non-standard in-progress errno. */
static int radio_wait_connected(RadioStream *rs, struct sockaddr_in *sa)
{
    int tries;
    RADIO_DBG(printf("radio-connect: session=%lu wait_connected enter fd=%ld host=%s\n", rs ? rs->session_id : 0, rs ? (long)rs->sock : -1L, rs ? rs->host : ""););
    /* ~6s budget at 40ms/poll; generous for a slow stream server. */
    for (tries = 0; tries < 150; tries++) {
        long e;
        int cr;
        if (radio_is_stopping(rs)) {
            RADIO_DBG(printf("radio-connect: session=%lu wait_connected stopping tries=%d\n", rs ? rs->session_id : 0, tries););
            return -1;
        }
        radio_backoff_sleep();
        if (radio_is_stopping(rs)) {
            RADIO_DBG(printf("radio-connect: session=%lu wait_connected stopping tries=%d\n", rs ? rs->session_id : 0, tries););
            return -1;
        }
        cr = connect(rs->sock, (struct sockaddr *)sa, sizeof(*sa));
        if (cr == 0) {
            RADIO_DBG(printf("radio-connect: session=%lu wait_connected connected tries=%d\n", rs ? rs->session_id : 0, tries););
            return 0;
        }
        e = radio_sock_errno();
        RADIO_DBG(printf("radio-connect: session=%lu wait_connected tries=%d cr=%d errno=%ld\n", rs ? rs->session_id : 0, tries, cr, e););
        if (e == EISCONN) {
            RADIO_DBG(printf("radio-connect: session=%lu wait_connected connected (EISCONN) tries=%d\n", rs ? rs->session_id : 0, tries););
            return 0;
        }
    }
    RADIO_DBG(printf("radio-connect: session=%lu wait_connected timed out\n", rs ? rs->session_id : 0););
    return -1;
}

/* Send the whole request on the (now non-blocking) socket, yielding on a
 * would-block partial send instead of failing. */
static int radio_send_all(RadioStream *rs, const char *buf, int len)
{
    int sent = 0, tries = 0;
    while (sent < len && tries < 150) {
        int r;
        if (radio_is_stopping(rs))
            return -1;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
        if (rs->isSSL && rs->ssl) {
            RADIO_DBG(printf("radio-ssl-write: session=%lu sslHandshakeDone=%d before SSL_write\n", rs->session_id, rs->sslHandshakeDone););
            if (rs->sslHandshakeDone != 1) {
                RADIO_DBG(printf("radio-ssl-write: ERROR session=%lu skipped SSL_write because handshake is incomplete sslHandshakeDone=%d\n", rs->session_id, rs->sslHandshakeDone););
                set_error(rs, "TLS handshake incomplete");
                close_current_socket(rs);
                return -1;
            }
            r = (int)SSL_write(rs->ssl, buf + sent, len - sent);
            if (r > 0) { sent += r; continue; }
            if (r < 0) {
                int e = SSL_get_error(rs->ssl, r);
                if (e == SSL_ERROR_WANT_WRITE || e == SSL_ERROR_WANT_READ) {
                    radio_backoff_sleep(); tries++; continue;
                }
            }
            return -1;
        }
#endif
        r = (int)send(rs->sock, (char *)buf + sent, len - sent, 0);
        if (r > 0) { sent += r; continue; }
        if (r < 0 && radio_would_block()) { radio_backoff_sleep(); tries++; continue; }
        return -1;
    }
    return sent == len ? 0 : -1;
}

static void set_status(RadioStream *rs, RadioStatus status) { if (rs && rs->status != RADIO_STATUS_ERROR && rs->status != RADIO_STATUS_CLOSED && rs->status != RADIO_STATUS_STOPPING) rs->status = status; }
static void radio_copy_string(char *dst, size_t dstSize, const char *src)
{
    if (!dst || dstSize == 0)
        return;
    if (!src)
        src = "";
    if (strlen(src) >= dstSize)
        RADIO_DBG(printf("radio-string: truncated copy dstSize=%lu srcLen=%lu src=\"%s\"\n", (unsigned long)dstSize, (unsigned long)strlen(src), src););
    snprintf(dst, dstSize, "%s", src);
    dst[dstSize - 1] = 0;
}

static void radio_copy_bytes(char *dst, size_t dstSize, const unsigned char *src, int srcLen)
{
    size_t copyLen;
    if (!dst || dstSize == 0)
        return;
    dst[0] = 0;
    if (!src || srcLen <= 0)
        return;
    copyLen = (size_t)srcLen;
    if (copyLen >= dstSize) {
        RADIO_DBG(printf("radio-string: truncated byte copy dstSize=%lu srcLen=%lu\n", (unsigned long)dstSize, (unsigned long)copyLen););
        copyLen = dstSize - 1;
    }
    memcpy(dst, src, copyLen);
    dst[copyLen] = 0;
}

static void set_error(RadioStream *rs, const char *msg) { if (rs) { radio_copy_string(rs->error,sizeof(rs->error),msg); rs->status = RADIO_STATUS_ERROR; RADIO_OPEN_DEBUG_PRINTF(("radio-open: %s\n", msg ? msg : "error")); } }
static void radio_ring_set_canary(RadioStream *rs);
static int radio_ring_check_canary(RadioStream *rs, const char *where);
static int ring_write(RadioStream *rs, const unsigned char *p, int n)
{
    int written = 0;
    if (!rs || !p || n <= 0 || !rs->ring || !rs->size) return 0;
    if (radio_ring_check_canary(rs, "before ring_write") < 0) return 0;
    while (written < n && rs->used < rs->size) {
        unsigned long freeBytes = rs->size - rs->used;
        unsigned long endBytes = rs->size - rs->wpos;
        unsigned long todo = (unsigned long)(n - written);
        unsigned long beforeWpos = rs->wpos;
        unsigned long beforeUsed = rs->used;
        int wrapped;
        if (todo > freeBytes) todo = freeBytes;
        if (todo > endBytes) todo = endBytes;
        if (todo == 0) break;
        memcpy(rs->ring + rs->wpos, p + written, (size_t)todo);
        rs->wpos += todo;
        if (rs->wpos >= rs->size) rs->wpos = 0;
        rs->used += todo;
        written += (int)todo;
        rs->ringLastWrite = todo;
        wrapped = (rs->wpos <= beforeWpos && todo > 0) ? 1 : 0;
        RADIO_DBG(printf("radio-ring: write session=%lu icy_state=%d metaint=%d bytes_to_ring=%lu wpos_before=%lu wpos_after=%lu fill_before=%lu fill_after=%lu free_before=%lu free_after=%lu wrapped=%d url=\"%s\"\n",
            rs->session_id, (int)rs->parseState, rs->metaint, todo, beforeWpos, rs->wpos,
            beforeUsed, rs->used, rs->size - beforeUsed, rs->size - rs->used, wrapped, rs->url););
        if (radio_ring_check_canary(rs, "after ring_write") < 0) break;
    }
    if (written == 0) {
        rs->ringLastWrite = 0;
        RADIO_DBG(printf("radio-ring: no write session=%lu icy_state=%d metaint=%d requested=%d fill=%lu ring_free=%lu wpos=%lu capacity=%lu url=\"%s\"\n",
            rs->session_id, (int)rs->parseState, rs->metaint, n, rs->used,
            rs->size > rs->used ? rs->size - rs->used : 0, rs->wpos, rs->size, rs->url););
    }
    return written;
}
static int ring_read(RadioStream *rs, unsigned char *p, int n) { int i=0; if(!rs||!p||n<=0)return 0; if(!rs->headerDone||!rs->decoderStarted){ RADIO_DBG(printf("radio-guard: ring_read refused session=%lu headerDone=%d decoderStarted=%d firstData=%d status=%d used=%lu\n", rs->session_id, rs->headerDone, rs->decoderStarted, rs->firstDataLogged, (int)rs->status, rs->used);); return 0; } if (radio_ring_check_canary(rs, "before ring_read") < 0) return 0; while (i<n && rs->used) { p[i++]=rs->ring[rs->rpos++]; if(rs->rpos>=rs->size)rs->rpos=0; rs->used--; } radio_ring_check_canary(rs, "after ring_read"); return i; }
static int ci_starts(const char *s,const char *p){ while(*p) { if(tolower((unsigned char)*s++)!=tolower((unsigned char)*p++)) return 0; } return 1; }
static int ci_equals(const char *a,const char *b){ while(*a&&*b){ if(tolower((unsigned char)*a++)!=tolower((unsigned char)*b++)) return 0; } return *a==0&&*b==0; }
static char *trim(char *s){ char *e; while(*s&&isspace((unsigned char)*s))s++; e=s+strlen(s); while(e>s&&isspace((unsigned char)e[-1]))*--e=0; return s; }

#define RADIO_RING_CANARY 0x5A
#define RADIO_RING_GUARD_BYTES 16UL
static void radio_ring_set_canary(RadioStream *rs)
{
    unsigned long i;
    if (!rs || !rs->ringAlloc || !rs->size) return;
    for (i = 0; i < RADIO_RING_GUARD_BYTES; i++) {
        rs->ringAlloc[i] = RADIO_RING_CANARY;
        rs->ringAlloc[RADIO_RING_GUARD_BYTES + rs->size + i] = RADIO_RING_CANARY;
    }
}
static int radio_ring_check_canary(RadioStream *rs, const char *where)
{
    unsigned long i;
    int bad = 0;
    if (!rs || !rs->ringAlloc || !rs->size) return 0;
    for (i = 0; i < RADIO_RING_GUARD_BYTES; i++) {
        if (rs->ringAlloc[i] != RADIO_RING_CANARY ||
            rs->ringAlloc[RADIO_RING_GUARD_BYTES + rs->size + i] != RADIO_RING_CANARY) {
            bad = 1;
            break;
        }
    }
    if (bad) {
        radio_poison_session_id = rs->session_id;
        radio_copy_string(radio_poison_url, sizeof(radio_poison_url), rs->url);
        RADIO_DBG(printf("radio-canary: CORRUPTED buffer=stream/audio ring session=%lu where=%s expected_capacity=%lu last_write_size=%lu codec=%s url=\"%s\"\n",
            rs->session_id, where ? where : "", rs->size, rs->ringLastWrite,
            ((ci_starts(rs->contentType,"audio/aac") || ci_starts(rs->contentType,"audio/aacp") || radio_contains_nocase(rs->path,"aac")) ? "AAC" : "MP3/unknown"),
            rs->url));
        rs->stopping = 1;
        rs->status = RADIO_STATUS_ERROR;
        set_error(rs, "Memory corruption detected - restart app");
        Radio_MarkMemoryPoisoned(where);
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
        /* One soak-test session hit this with no SSL_ERROR_SSL involved at
         * all -- a perfectly healthy, actively-streaming connection whose
         * ring buffer was found corrupted mid-stream, which later crashed
         * inside an unprotected SSL_CTX_free() because nothing had marked
         * this session poisoned. Any detected ring corruption, regardless
         * of how it got there, means AmiSSL-adjacent heap state for this
         * session may already be damaged -- so treat it exactly like the
         * SSL_ERROR_SSL case and skip further SSL_free()/SSL_CTX_free()/
         * CleanupAmiSSL() calls for it. */
        rs->sslStatePoisoned = 1;
        radio_amissl_task_poisoned = 1;
        Radio_MarkTlsPoisoned("ring corruption during AmiSSL session");
#endif
        return -1;
    }
    return 0;
}

static int parse_url(RadioStream *rs, const char *url)
{
    const char *p, *slash, *colon;
    int hl, default_port;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    if (!url) return -1;
    if (strncmp(url, "https://", 8) == 0) {
        rs->isSSL = 1; default_port = 443; p = url + 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        rs->isSSL = 0; default_port = 80; p = url + 7;
    } else {
        return -1;
    }
#else
    if (!url || strncmp(url, "http://", 7)) return -1;
    default_port = 80; p = url + 7;
#endif
    slash = strchr(p, '/');
    if (!slash) slash = p + strlen(p);
    colon = (const char *)memchr(p, ':', (size_t)(slash - p));
    hl = (int)((colon ? colon : slash) - p);
    if (hl <= 0 || hl >= (int)sizeof(rs->host)) return -1;
    memcpy(rs->host, p, (size_t)hl); rs->host[hl] = 0;
    rs->port = colon ? atoi(colon + 1) : default_port;
    if (rs->port <= 0) rs->port = default_port;
    radio_copy_string(rs->path, sizeof(rs->path), *slash ? slash : "/");
    radio_copy_string(rs->url, sizeof(rs->url), url);
    return 0;
}

static void reset_parser(RadioStream *rs)
{
    rs->headerDone = 0; rs->headerLen = 0; rs->header[0] = 0;
    rs->parseState = RADIO_PARSE_HEADER;
    rs->metaint = 0; rs->audioUntilMeta = 0;
    rs->metaLen = rs->metaGot = rs->metaLeft = 0;
    rs->rpos = rs->wpos = rs->used = 0;
    rs->zeroBytePumps = 0;
    rs->firstDataLogged = 0;
    rs->contentType[0] = 0; rs->bitrate = 0;
    rs->stationName[0] = 0; rs->genre[0] = 0; rs->streamUrl[0] = 0;
    rs->title[0] = 0;
}

static int connect_http(RadioStream *rs){
    struct sockaddr_in sa; char req[512]; int n; int cr;
#if defined(AMIGA_M68K)
    if(!SocketBase) SocketBase=OpenLibrary("bsdsocket.library",4); if(!SocketBase){ set_error(rs,"bsdsocket.library unavailable"); RADIO_OPEN_DEBUG_PRINTF(("radio-open: bsdsocket open failed\n")); return -1; }
#endif
    if (radio_is_stopping(rs)) return -1;
    /* Resolve once and cache it; gethostbyname() is blocking and would freeze
     * the emulator on every reconnect if we re-resolved each time. */
    if(rs->haveHostAddr){
        unsigned long addr_be;
        char addr_text[16];
        memset(&addr_be, 0, sizeof(addr_be));
        memcpy(&addr_be, &rs->hostAddr.s_addr, sizeof(rs->hostAddr.s_addr));
        radio_format_ipv4_be(addr_be, addr_text, (int)sizeof(addr_text));
        RADIO_DBG(printf("radio-dns: session=%lu using cached probe DNS %s\n", rs->session_id, addr_text););
    } else {
        struct hostent *he;
        RADIO_DBG(printf("radio-dns: WARNING blocking DNS lookup in playback child host=%s\n", rs->host););
        he=gethostbyname(rs->host);
        if(!he || !he->h_addr){ set_error(rs,"cannot resolve stream host"); RADIO_OPEN_DEBUG_PRINTF(("radio-open: DNS failed for %s\n", rs->host)); return -1; }
        memcpy(&rs->hostAddr, he->h_addr, sizeof(rs->hostAddr));
        rs->haveHostAddr=1;
    }
    if (radio_is_stopping(rs)) return -1;
    rs->sock=socket(AF_INET,SOCK_STREAM,0); if(rs->sock!=RADIO_INVALID_SOCKET){ rs->socketClosed = 0; radio_open_socket_count++; radio_playback_open_socket_count++; RADIO_DBG(printf("radio-socket: playback socket opened session=%lu host=%s fd=%ld open_socket_count=%ld playback_open_socket_count=%ld\n", rs->session_id, rs->host, (long)rs->sock, radio_open_socket_count, radio_playback_open_socket_count);) } if(rs->sock==RADIO_INVALID_SOCKET){ radio_log_socket_failure(rs, "playback", "socket"); set_error(rs,"Cannot create socket - TCP stack may still be releasing previous stream"); return -1; }
    /* Go non-blocking BEFORE connect so the connect never stalls the machine. */
    radio_set_nonblocking(rs->sock);
    memset(&sa,0,sizeof(sa)); sa.sin_family=AF_INET; sa.sin_port=htons((unsigned short)rs->port); sa.sin_addr=rs->hostAddr;
    if (radio_is_stopping(rs)) { close_current_socket(rs); return -1; }
    RADIO_DBG(printf("radio-connect: session=%lu initial connect() fd=%ld host=%s port=%d\n", rs->session_id, (long)rs->sock, rs->host, rs->port););
    cr=connect(rs->sock,(struct sockaddr*)&sa,sizeof(sa));
    RADIO_DBG(printf("radio-connect: session=%lu initial connect() cr=%d errno=%ld\n", rs->session_id, cr, cr < 0 ? radio_sock_errno() : 0L););
    if(cr<0 && radio_wait_connected(rs,&sa)!=0){ close_current_socket(rs); set_error(rs,"TCP connect failed"); RADIO_OPEN_DEBUG_PRINTF(("radio-open: connect failed to %s:%d\n", rs->host, rs->port)); return -1; }
    rs->streamStateFlags |= TRANSPORT_CONNECTED;
    if (radio_is_stopping(rs)) { close_current_socket(rs); return -1; }
    RADIO_DBG(printf("radio-connect: session=%lu TCP connected, starting SSL/HTTP request phase isSSL=%d\n", rs->session_id, rs->isSSL););
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    if (rs->isSSL) {
        rs->sslHandshakeDone = 0;
        if (radio_ssl_connect(rs) != 0) { close_current_socket(rs); return -1; }
        rs->streamStateFlags |= TLS_DONE;
        if (radio_is_stopping(rs)) { close_current_socket(rs); return -1; }
    }
#endif
    RADIO_DBG(printf("radio-http: session=%lu starting HTTP request phase isSSL=%d sslHandshakeDone=%d\n", rs->session_id, rs->isSSL, rs->sslHandshakeDone););
    n=snprintf(req,sizeof(req),"GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: BoingPlayer/0.1 AmigaOS\r\nIcy-MetaData: 1\r\nConnection: close\r\n\r\n",rs->path,rs->host);
    if(radio_send_all(rs,req,n)!=0){ close_current_socket(rs); set_error(rs, rs->isSSL ? "HTTPS read failed" : "cannot send HTTP request"); return -1; }
    reset_parser(rs);
    rs->decoderStarted = rs->playbackStarted = 0;
    rs->streamStateFlags &= (STREAM_ALLOCATED | TRANSPORT_CONNECTED | TLS_DONE);
    return 0;
}

static void radio_abort_current_socket(RadioStream *rs)
{
    if (!rs) return;
    radio_stream_magic_valid(rs, "radio_abort_current_socket");
    if (rs->sock != RADIO_INVALID_SOCKET && !rs->socketClosed) {
        long closing_fd = (long)rs->sock;
        long before_all = radio_open_socket_count;
        long before_playback = radio_playback_open_socket_count;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
        /* rs->ssl (if any) still has this fd wired into its BIO via
         * SSL_set_fd(), but SSL_free() for this session doesn't run until
         * much later (close_current_socket_mode(), after Radio_Close()) --
         * this is the immediate abort path used to unblock a live session
         * right away. In the gap between closing the fd here and SSL_free()
         * finally running, bsdsocket.library is free to hand the same fd
         * number to an unrelated socket opened by another task sharing this
         * SocketBase (e.g. a probe/favicon fetch). Detach the BIO's fd now,
         * before it goes stale, so SSL_free()'s internal cleanup can never
         * later touch a since-reused fd -- this is the SSL_free()-crashes
         * (AN_BadFreeAddr) case tied to aborting a live/playing stream. */
        if (rs->ssl) {
            BIO *rbio = SSL_get_rbio(rs->ssl);
            if (rbio) BIO_set_fd(rbio, -1, BIO_NOCLOSE);
        }
#endif
        RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: abort CloseSocket start fd=%ld\n", (long)rs->sock));
        rs->socket_close_count++;
        radio_close_socket(rs->sock);
        rs->sock = RADIO_INVALID_SOCKET;
        rs->socketClosed = 1;
        if (radio_open_socket_count > 0) radio_open_socket_count--;
        if (radio_playback_open_socket_count > 0) radio_playback_open_socket_count--;
        RADIO_DBG(printf("radio-socket: playback socket close session=%lu fd=%ld open_socket_count %ld->%ld playback_open_socket_count %ld->%ld\n", rs->session_id, closing_fd, before_all, radio_open_socket_count, before_playback, radio_playback_open_socket_count););
        RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: abort CloseSocket done\n"));
        RADIO_STOP_DEBUG_PRINTF(("radio-stop: socket aborted\n"));
    } else {
        RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: abort CloseSocket skipped fd=-1\n"));
    }
}

static void close_current_socket_mode(RadioStream *rs, RadioCloseMode mode)
{
    long before;
    if (!rs) return;
    radio_stream_magic_valid(rs, "close_current_socket");
    before = radio_open_socket_count;
    RADIO_DBG(printf("radio-cleanup: close mode=%s session=%lu status=%d fd=%ld open_socket_count_before=%ld\n",
        radio_close_mode_name(mode), rs->session_id, (int)rs->status, (long)rs->sock, before););
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    radio_ssl_close_stream_mode(rs, mode);
#endif
    radio_abort_current_socket(rs);
    RADIO_DBG(printf("radio-cleanup: close mode=%s session=%lu complete open_socket_count_after=%ld\n",
        radio_close_mode_name(mode), rs->session_id, radio_open_socket_count););
}
/* Safe default: always abort (skip SSL_shutdown).  Every call site below
 * this comment represents a failed/aborted transport (connect/handshake
 * failure, send/recv error, start timeout, stop before healthy playback) --
 * exactly the states where SSL_shutdown() can provoke a recoverable AmiSSL
 * alert.  The one deliberate exception is Radio_RequestStop()'s explicit
 * graceful pre-close of a still-healthy RADIO_STATUS_PLAYING session. */
static void close_current_socket(RadioStream *rs) { close_current_socket_mode(rs, RADIO_CLOSE_ABORT); }

static int reconnect_http(RadioStream *rs)
{
    close_current_socket(rs);
    if (radio_is_stopping(rs)) { if (rs) rs->status = RADIO_STATUS_CLOSED; return -1; }
    if (rs->reconnectAttempts >= RADIO_RECONNECT_MAX) { set_error(rs,"radio reconnect attempts exhausted"); return -1; }
    if (rs->reconnectDelay > 0) { rs->reconnectDelay--; set_status(rs, RADIO_STATUS_RECONNECTING); radio_backoff_sleep(); return 0; }
    rs->reconnectAttempts++;
    set_status(rs, rs->reconnectAttempts == 1 ? RADIO_STATUS_CONNECTING : RADIO_STATUS_RECONNECTING);
    if (connect_http(rs) == 0) { set_status(rs, RADIO_STATUS_BUFFERING); return 1; }
    rs->reconnectDelay = RADIO_RECONNECT_BACKOFF_PUMPS * rs->reconnectAttempts;
    set_status(rs, RADIO_STATUS_RECONNECTING);
    return 0;
}

static void parse_headers(RadioStream *rs,char *h){ if (radio_stream_magic_valid(rs, "parse_headers") < 1) return; char *line=strtok(h,"\r\n"); int code=0; if(line && ci_starts(line,"ICY")) code=200; else if(line && ci_starts(line,"HTTP/")) sscanf(line,"HTTP/%*s %d",&code); else { set_error(rs,"invalid HTTP stream response"); RADIO_OPEN_DEBUG_PRINTF(("radio-open: HTTP header failed\n")); return; } if(code<200||code>299){ char msg[64]; sprintf(msg,"HTTP %d stream error",code); set_error(rs,msg); RADIO_OPEN_DEBUG_PRINTF(("radio-open: HTTP header failed status %d\n", code)); return; } while((line=strtok(NULL,"\r\n"))){ char *v=strchr(line,':'); if(!v) continue; *v++=0; line=trim(line); v=trim(v); if(ci_equals(line,"Content-Type")) radio_copy_string(rs->contentType,sizeof(rs->contentType),v); else if(ci_equals(line,"icy-metaint")){ rs->metaint=atoi(v); rs->audioUntilMeta=rs->metaint; } else if(ci_equals(line,"icy-br")) rs->bitrate=atoi(v); else if(ci_equals(line,"icy-name")) radio_copy_string(rs->stationName,sizeof(rs->stationName),v); else if(ci_equals(line,"icy-genre")) radio_copy_string(rs->genre,sizeof(rs->genre),v); else if(ci_equals(line,"icy-url")) radio_copy_string(rs->streamUrl,sizeof(rs->streamUrl),v); } if(rs->contentType[0] && (ci_starts(rs->contentType,"application/vnd.apple.mpegurl") || ci_starts(rs->contentType,"application/x-mpegurl"))) { set_error(rs,"HLS stream not supported"); RADIO_OPEN_DEBUG_PRINTF(("radio-open: HLS content type unsupported: %s\n", rs->contentType)); return; } RADIO_OPEN_DEBUG_PRINTF(("radio-open: final URL=%s content-type=%s URL codec hint=%s final selected codec=%s\n",
    rs->url, rs->contentType,
    radio_contains_nocase(rs->path,"mp3") ? "MP3" : (radio_contains_nocase(rs->path,"aac") ? "AAC" : "none"),
    (ci_starts(rs->contentType,"audio/mpeg") || ci_starts(rs->contentType,"audio/mp3") || radio_contains_nocase(rs->path,"mp3")) ? "MP3" :
    ((ci_starts(rs->contentType,"audio/aac") || ci_starts(rs->contentType,"audio/aacp") || radio_contains_nocase(rs->path,"aac")) ? "AAC" : "unknown"))); }

static void parse_meta(RadioStream *rs,const unsigned char *m,int n)
{
    static const char key[] = "StreamTitle='";
    const unsigned char *p, *end;
    char oldTitle[128];
    int i, keyLen = (int)sizeof(key) - 1;
    if (!rs || !m || n <= 0)
        return;
    if (!radio_stream_magic_valid(rs, "parse_meta"))
        return;
    if (n >= RADIO_META_MAX)
        RADIO_DBG(printf("radio-icy: metadata truncated session=%lu metaLen=%d capacity=%lu station=\"%s\" url=\"%s\"\n", rs->session_id, n, (unsigned long)RADIO_META_MAX, rs->stationName, rs->url););
    end = m + n;
    for (i = 0; i + keyLen <= n; i++) {
        if (!memcmp(m + i, key, (size_t)keyLen)) {
            p = m + i + keyLen;
            for (i = 0; p + i < end && p[i] != '\''; i++)
                ;
            radio_copy_string(oldTitle, sizeof(oldTitle), rs->title);
            radio_copy_bytes(rs->title, sizeof(rs->title), p, i);
            if (strcmp(oldTitle, rs->title) != 0)
                RADIO_DBG(printf("radio-resource: session=%lu ICY metadata updated (fixed buffer, active_icy_metadata_count=%ld)\n", rs->session_id, radio_active_icy_metadata_count););
            break;
        }
    }
}

static int process_bytes(RadioStream *rs, const unsigned char *b, int n)
{
    int i = 0;
    if (!rs || !b || n <= 0) return 0;
    while (i < n) {
        int avail;
        if (rs->stopping) break;
        if (rs->parseState == RADIO_PARSE_HEADER) {
            if (rs->headerLen >= RADIO_HEADER_MAX - 1) { set_error(rs,"HTTP header too large"); return -1; }
            rs->header[rs->headerLen++] = (char)b[i++]; rs->header[rs->headerLen] = 0;
            if (rs->headerLen >= 4 && !memcmp(rs->header + rs->headerLen - 4, "\r\n\r\n", 4)) {
                RADIO_DBG(printf("radio-http-diag: session=%lu raw HTTP response header (%d bytes) follows:\n%s--- end of header ---\n", rs->session_id, rs->headerLen, rs->header););
                rs->headerDone = 1; rs->streamStateFlags |= HEADER_DONE; parse_headers(rs, rs->header); if (rs->status == RADIO_STATUS_ERROR) return -1; rs->parseState = RADIO_PARSE_AUDIO;
            }
            continue;
        }

        avail = n - i;
        if (rs->metaint > 0 && rs->parseState == RADIO_PARSE_AUDIO && rs->audioUntilMeta <= 0)
            rs->parseState = RADIO_PARSE_META_LEN;

        RADIO_DBG(printf("radio-icy: loop session=%lu icy_state=%d metaint=%d audio_until_meta=%d meta_remaining=%d src_avail=%d fill=%lu ring_free=%lu wpos=%lu url=\"%s\"\n",
            rs->session_id, (int)rs->parseState, rs->metaint, rs->audioUntilMeta,
            rs->metaLeft, avail, rs->used, rs->size > rs->used ? rs->size - rs->used : 0, rs->wpos, rs->url););

        if (rs->parseState == RADIO_PARSE_META_LEN) {
            int lenByte = b[i++];
            rs->metaLen = lenByte * 16;
            rs->metaGot = 0;
            rs->metaLeft = rs->metaLen;
            RADIO_DBG(printf("radio-icy: metadata length session=%lu len_byte=%d meta_len=%d src_avail_after=%d metaint=%d url=\"%s\"\n",
                rs->session_id, lenByte, rs->metaLen, n - i, rs->metaint, rs->url););
            if (rs->metaLen > RADIO_META_MAX)
                RADIO_DBG(printf("radio-icy: metadata payload exceeds buffer session=%lu metaLen=%d capacity=%lu; truncating parse copy station=\"%s\" url=\"%s\"\n", rs->session_id, rs->metaLen, (unsigned long)RADIO_META_MAX, rs->stationName, rs->url););
            if (rs->metaLen == 0) {
                rs->audioUntilMeta = rs->metaint;
                rs->parseState = RADIO_PARSE_AUDIO;
            } else {
                rs->parseState = RADIO_PARSE_META_PAYLOAD;
            }
            continue;
        }

        if (rs->parseState == RADIO_PARSE_META_PAYLOAD) {
            int take = rs->metaLeft < avail ? rs->metaLeft : avail;
            int copy = take;
            if (copy > RADIO_META_MAX - 1 - rs->metaGot) copy = RADIO_META_MAX - 1 - rs->metaGot;
            if (copy > 0) {
                memcpy(rs->meta + rs->metaGot, b + i, (size_t)copy);
                rs->metaGot += copy;
            }
            i += take;
            rs->metaLeft -= take;
            RADIO_DBG(printf("radio-icy: metadata payload session=%lu consumed=%d copied=%d meta_remaining=%d src_avail_after=%d url=\"%s\"\n",
                rs->session_id, take, copy, rs->metaLeft, n - i, rs->url););
            if (rs->metaLeft <= 0) {
                if (rs->metaGot > 0) parse_meta(rs, rs->meta, rs->metaGot);
                rs->audioUntilMeta = rs->metaint;
                rs->parseState = RADIO_PARSE_AUDIO;
            }
            continue;
        }

        if (rs->parseState == RADIO_PARSE_AUDIO) {
            int audioAvail = avail;
            int wanted, wrote;
            if (rs->metaint > 0 && rs->audioUntilMeta < audioAvail) audioAvail = rs->audioUntilMeta;
            wanted = audioAvail;
            if (wanted > 0 && rs->size > rs->used && (unsigned long)wanted > rs->size - rs->used)
                wanted = (int)(rs->size - rs->used);
            if (wanted <= 0) {
                RADIO_DBG(printf("radio-ring: full/wait session=%lu icy_state=%d metaint=%d audio_until_meta=%d src_avail=%d fill=%lu ring_free=%lu wpos=%lu capacity=%lu url=\"%s\"\n",
                    rs->session_id, (int)rs->parseState, rs->metaint, rs->audioUntilMeta, avail,
                    rs->used, rs->size > rs->used ? rs->size - rs->used : 0, rs->wpos, rs->size, rs->url););
                break;
            }
            RADIO_DBG(printf("radio-icy: audio write plan session=%lu metaint=%d audio_until_meta_before=%d src_avail=%d bytes_to_ring=%d fill_before=%lu ring_free_before=%lu wpos_before=%lu url=\"%s\"\n",
                rs->session_id, rs->metaint, rs->audioUntilMeta, avail, wanted, rs->used,
                rs->size > rs->used ? rs->size - rs->used : 0, rs->wpos, rs->url););
            wrote = ring_write(rs, b + i, wanted);
            if (wrote > 0 && rs->headerDone && !rs->decoderStarted) { rs->decoderStarted = 1; rs->streamStateFlags |= DECODER_STARTED; radio_active_decoder_count++; RADIO_DBG(printf("radio-resource: session=%lu decoder path started active_decoder_count=%ld\n", rs->session_id, radio_active_decoder_count);); }
            i += wrote;
            if (rs->metaint > 0) rs->audioUntilMeta -= wrote;
            RADIO_DBG(printf("radio-icy: audio write done session=%lu wrote=%d audio_until_meta_after=%d src_avail_after=%d fill_after=%lu ring_free_after=%lu wpos_after=%lu url=\"%s\"\n",
                rs->session_id, wrote, rs->audioUntilMeta, n - i, rs->used,
                rs->size > rs->used ? rs->size - rs->used : 0, rs->wpos, rs->url););
            if (wrote <= 0) break;
            continue;
        }
    }
    return 0;
}

static int radio_note_start_wait(RadioStream *rs, const char *message)
{
    if (!rs || rs->everPlayed) return 0;
    rs->startPumps++;
    if (rs->startPumps >= RADIO_START_TIMEOUT_PUMPS) {
        set_error(rs, message);
        close_current_socket(rs);
        return -1;
    }
    return 0;
}

RadioStream *Radio_OpenWithHostAddr(const char *url, int haveHostAddr, unsigned long hostAddrBe)
{
    RadioStream *rs = (RadioStream *)calloc(1, sizeof(*rs));
    if (!rs) return NULL;
    /* radio_stream_magic_valid() (used by close_current_socket(),
     * parse_headers(), parse_meta(), ...) compares against this field to
     * catch a corrupted/stale RadioStream* before it gets dereferenced --
     * that check is only meaningful if the magic is actually stamped on a
     * freshly allocated stream, so it must happen before anything else can
     * observe or act on rs. */
    rs->magic = RADIO_STREAM_MAGIC;
    if (!radio_atexit_registered) { atexit(radio_app_exit_report); radio_atexit_registered = 1; }
    radio_reset_session_state(rs);
    rs->session_id = radio_next_session_id++;
    if (radioMemoryPoisoned) {
        rs->status = RADIO_STATUS_ERROR;
        radio_copy_string(rs->url, sizeof(rs->url), url ? url : "");
        set_error(rs, "Memory corruption detected - restart app");
        RADIO_OPEN_DEBUG_PRINTF(("radio-open: refused stream start after memory poison session=%lu url=\"%s\"\n", rs->session_id, rs->url));
        return rs;
    }
    if (url && !strncmp(url, "https://", 8) && Radio_IsTlsPoisoned()) {
        rs->status = RADIO_STATUS_ERROR;
        radio_copy_string(rs->url, sizeof(rs->url), url);
        set_error(rs, Radio_TlsPoisonedMessage());
        RADIO_OPEN_DEBUG_PRINTF(("radio-open: refused HTTPS stream start after AmiSSL poison session=%lu url=\"%s\"\n", rs->session_id, rs->url));
        return rs;
    }
    if (haveHostAddr) {
        memcpy(&rs->hostAddr, &hostAddrBe, sizeof(rs->hostAddr));
        rs->haveHostAddr = 1;
    }
    radio_active_stream_sessions++;
    radio_active_stream_tasks++;
    rs->streamStateFlags |= STREAM_ALLOCATED;
    RADIO_DBG(printf("radio-resource: session=%lu stream session/task allocated active_stream_sessions=%ld active_stream_tasks=%ld active_decoder_count=%ld\n", rs->session_id, radio_active_stream_sessions, radio_active_stream_tasks, radio_active_decoder_count););
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    RADIO_DBG(printf("radio-ssl-diag: Radio_Open session=%lu url=\"%s\" inherited AmiSSL state: initialized=%d base=%p ext=%p master=%p ssl_count=%ld ssl_ctx_count=%ld\n",
        rs->session_id, url ? url : "(null)", radio_amissl_initialized, (void *)AmiSSLBase, (void *)AmiSSLExtBase, (void *)AmiSSLMasterBase, radio_active_ssl_count, radio_active_ssl_ctx_count));
#endif
    radio_debug_mem_report(rs->session_id, "before stream start");
    radio_poison_session_id = rs->session_id;
    radio_copy_string(radio_poison_url, sizeof(radio_poison_url), url ? url : "");
    if (Radio_CheckMiniMem("before ring buffer allocated") > 0) {
        set_error(rs, "Memory corruption detected - restart app");
        RADIO_OPEN_DEBUG_PRINTF(("radio-open: refused ring allocation after MiniMem corruption session=%lu url=\"%s\"\n", rs->session_id, radio_poison_url));
        return rs;
    }
    rs->status = RADIO_STATUS_CONNECTING;
    rs->size = RADIO_RING_BYTES;
    RADIO_DBG(printf("radio-resource: session=%lu ring buffer owner url=\"%s\" allocFile=radio_stream.c allocLine=Radio_OpenWithHostAddr\n",
        rs->session_id, radio_poison_url);)
    rs->ringAlloc = (unsigned char *)malloc(rs->size + 2UL * RADIO_RING_GUARD_BYTES);
    if (rs->ringAlloc) { rs->ring = rs->ringAlloc + RADIO_RING_GUARD_BYTES; radio_ring_set_canary(rs); }
    /* Printed unconditionally (not RADIO_DBG-gated) so it's visible even if
     * a build is run without the console attached to a log file -- this is
     * the address to arm a live debugger watchpoint on when chasing the
     * ring-corruption-with-no-detectable-trigger case: front guard is
     * [ringAlloc, ringAlloc+16), rear guard is
     * [ringAlloc+16+size, ringAlloc+32+size). */
    if (rs->ringAlloc)
        printf("radio-resource: session=%lu ring buffer allocated at %p (front guard %p..%p, data %p..%p, rear guard %p..%p)\n",
            rs->session_id, (void *)rs->ringAlloc,
            (void *)rs->ringAlloc, (void *)(rs->ringAlloc + RADIO_RING_GUARD_BYTES),
            (void *)rs->ring, (void *)(rs->ring + rs->size),
            (void *)(rs->ringAlloc + RADIO_RING_GUARD_BYTES + rs->size),
            (void *)(rs->ringAlloc + 2UL * RADIO_RING_GUARD_BYTES + rs->size));
    /* The soak test caught the ring's malloc-guard header already zeroed
     * (magic=00000000) by session 4, in a run where headerDone was still 0
     * -- meaning ring_write()/ring_read() were never even called this
     * session, ruling those out as the source. Checking immediately after
     * this fresh allocation (before anything touches the buffer) tells us
     * whether malloc() is handing back an already-damaged block -- pointing
     * at heap/free-list corruption from an earlier session -- or whether
     * this allocation starts clean and something corrupts it afterward. */
    if (Radio_CheckMiniMem("after ring buffer allocated") > 0) {
        set_error(rs, "Memory corruption detected - restart app");
        return rs;
    }
    if (rs->ring) { rs->audioInitialized = 1; radio_active_stream_buffer_count++; radio_active_audio_buffer_count++; RADIO_DBG(printf("radio-resource: session=%lu stream/audio buffer allocated active_stream_buffer_count=%ld active_audio_buffer_count=%ld\n", rs->session_id, radio_active_stream_buffer_count, radio_active_audio_buffer_count)); }
    if (!rs->ring) {
        rs->size = 0;
        set_error(rs, "not enough memory for radio buffer");
        RADIO_OPEN_DEBUG_PRINTF(("radio-open: Radio_Open returning error\n"));
        return rs;
    }
    if (radio_url_looks_hls(url)) {
        set_error(rs, "HLS stream not supported");
        radio_debug_mem_report(rs->session_id, "after failed probe cleanup");
        radio_resource_summary(rs, "after failed probe cleanup");
        RADIO_OPEN_DEBUG_PRINTF(("radio-open: HLS URL rejected before direct playback\n"));
        return rs;
    }
    if (parse_url(rs, url)) {
        set_error(rs,
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
            "only direct http:// or https:// stream URLs are supported"
#else
            "only direct http:// stream URLs are supported"
#endif
        );
        radio_debug_mem_report(rs->session_id, "after failed probe cleanup");
        radio_resource_summary(rs, "after failed probe cleanup");
        RADIO_OPEN_DEBUG_PRINTF(("radio-open: Radio_Open returning error\n"));
        return rs;
    }
    if (connect_http(rs) == 0) {
        rs->status = RADIO_STATUS_BUFFERING;
        radio_debug_mem_report(rs->session_id, "after HTTP request phase started");
    }
    else {
        close_current_socket(rs);
        if (rs->status != RADIO_STATUS_ERROR)
            set_error(rs, rs->error[0] ? rs->error : "cannot open radio stream");
        radio_debug_mem_report(rs->session_id, "after failed probe cleanup");
        radio_resource_summary(rs, "after failed probe cleanup");
        RADIO_OPEN_DEBUG_PRINTF(("radio-open: Radio_Open returning error\n"));
    }
    return rs;
}

RadioStream *Radio_Open(const char *url)
{
    return Radio_OpenWithHostAddr(url, 0, 0);
}
void Radio_RequestStop(RadioStream *rs)
{
    RadioCloseMode mode;
    if (!rs) return;
    radio_debug_mem_report(rs->session_id, "before stop");
    rs->stop_request_count++;
    RADIO_STOP_DEBUG_PRINTF(("radio-stop: session=%lu stop requested count=%u status=%d fd=%ld\n", rs->session_id, rs->stop_request_count, (int)rs->status, (long)rs->sock));
    if (rs->status == RADIO_STATUS_CLOSED) return;
    /* Always abort, never call SSL_shutdown() here, even for a healthy
     * RADIO_STATUS_PLAYING session: a prior revision tried a "graceful"
     * SSL_shutdown() in exactly that case to write a clean TLS close_notify
     * before tearing the socket down, and that is what has been producing
     * AN_BadFreeAddr (0x0100000F, "memory header not located") when
     * interrupting a playing HTTPS stream to switch to another station --
     * SSL_shutdown() on a still-live AmiSSL session from a stop/interrupt
     * request is not safe here, on a non-blocking socket, from this call
     * path.  Skipping it just means the peer sees an unclean TCP close
     * instead of a TLS close_notify, which is harmless and exactly what
     * every other stop/error/timeout path already does. */
    mode = RADIO_CLOSE_ABORT;
    RADIO_DBG(printf("radio-cleanup: close mode=%s session=%lu status=%d (Radio_RequestStop)\n", radio_close_mode_name(mode), rs->session_id, (int)rs->status););
    rs->stopping = 1;
    rs->reconnectAttempts = RADIO_RECONNECT_MAX;
    rs->reconnectDelay = 0;
    rs->status = RADIO_STATUS_STOPPING;
    radio_abort_current_socket(rs);
    RADIO_STOP_DEBUG_PRINTF(("radio-stop: marked stopping\n"));
}
void Radio_Close(RadioStream *rs)
{
    if (!rs) return;
    RADIO_DBG(printf("radio-http-diag: Radio_Close session=%lu status=%d everPlayed=%d headerDone=%d firstData=%d used=%lu metaint=%d reconnectAttempts=%d startPumps=%d stopping=%d stopReq=%u contentType=\"%s\" host=\"%s\" path=\"%s\" error=\"%s\"\n",
        rs->session_id, (int)rs->status, rs->everPlayed, rs->headerDone, rs->firstDataLogged, rs->used, rs->metaint,
        rs->reconnectAttempts, rs->startPumps, rs->stopping, rs->stop_request_count,
        rs->contentType, rs->host, rs->path, rs->error));
    RADIO_STOP_DEBUG_PRINTF(("radio-stop: Radio_Close entered session=%lu\n", rs->session_id));
    rs->cleanup_count++;
    rs->cleanupDone = 1;
    if (rs->cleanup_count > 1) radio_duplicate_cleanup_warning(rs, "session cleanup", rs->cleanup_count);
    Radio_RequestStop(rs);
    close_current_socket(rs);
    rs->status = RADIO_STATUS_CLOSED;
    rs->stream_buffer_free_count++;
    rs->audio_buffer_free_count++;
    if (rs->stream_buffer_free_count > 1) radio_duplicate_cleanup_warning(rs, "stream buffer free", rs->stream_buffer_free_count);
    else { if (rs->ringAlloc) {
        /* radio_ring_check_canary() returning -1 means the guard bytes
         * around this block are already corrupted -- in a HEAPGUARD debug
         * build, MiniMem_Free() defensively refuses to free a block whose
         * header doesn't look valid, but a release build has no such
         * safety net: calling the real free() on an already-corrupted
         * block risks smashing the allocator's own free-list instead of
         * just leaking one buffer. Leak it deliberately instead. */
        if (radio_ring_check_canary(rs, "before cleanup") == 0)
            free(rs->ringAlloc);
        else
            RADIO_DBG(printf("radio-cleanup: session=%lu url=\"%s\" ring buffer free skipped (corrupted), quarantining/leaking to avoid smashing the allocator\n", rs->session_id, rs->url));
        if (radio_active_stream_buffer_count > 0) radio_active_stream_buffer_count--;
    } }
    if (rs->audioInitialized || rs->playbackStarted) {
        if (rs->audio_buffer_free_count > 1) radio_duplicate_cleanup_warning(rs, "audio buffer free", rs->audio_buffer_free_count);
        else { if (radio_active_audio_buffer_count > 0) radio_active_audio_buffer_count--; }
    }
    if (rs->decoderStarted) {
        rs->decoder_free_count++;
        if (rs->decoder_free_count > 1) radio_duplicate_cleanup_warning(rs, "decoder free", rs->decoder_free_count);
        else { if (radio_active_decoder_count > 0) radio_active_decoder_count--; }
    } else {
        RADIO_DBG(printf("radio-cleanup: session=%lu decoder cleanup skipped (decoderStarted=0 headerDone=%d firstData=%d)\n", rs->session_id, rs->headerDone, rs->firstDataLogged););
    }
    rs->ring = NULL; rs->ringAlloc = NULL;
    rs->size = rs->used = rs->rpos = rs->wpos = 0;
    rs->task_exit_count++;
    if (radio_active_stream_tasks > 0) radio_active_stream_tasks--;
    if (radio_active_stream_sessions > 0) radio_active_stream_sessions--;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    /* bsdsocket.library and amisslmaster.library are process-wide resources
     * kept open for the whole app run, but the per-task AmiSSL context
     * (OpenAmiSSL()/InitAmiSSL() in radio_ssl_global_init()) must be closed
     * by this task before it exits -- see the comment there. Each new
     * playback session runs in a freshly spawned child task/process, so
     * this always correctly runs once per task. */
    radio_ssl_free_ctx(rs);
    rs->amissl_cleanup_count++;
    radio_ssl_global_cleanup();
#endif
    radio_debug_mem_report(rs->session_id, "after stop cleanup");
    radio_resource_summary(rs, "after stop cleanup");
    RADIO_STOP_DEBUG_PRINTF(("radio-stop: stream task exiting / Radio_Close exited session=%lu task_exit_count=%u cleanup_count=%u stop_request_count=%u ssl_free_count=%u socket_close_count=%u decoder_free_count=%u\n", rs->session_id, rs->task_exit_count, rs->cleanup_count, rs->stop_request_count, rs->ssl_free_count, rs->socket_close_count, rs->decoder_free_count));
    free(rs);
}

void Radio_GetNetworkStats(long *active_stream_sessions, long *active_stream_tasks,
    long *open_socket_count, long *active_ssl_count, long *active_ssl_ctx_count)
{
    if (active_stream_sessions) *active_stream_sessions = radio_active_stream_sessions;
    if (active_stream_tasks) *active_stream_tasks = radio_active_stream_tasks;
    if (open_socket_count) *open_socket_count = radio_open_socket_count;
    if (active_ssl_count) *active_ssl_count = radio_active_ssl_count;
    if (active_ssl_ctx_count) *active_ssl_ctx_count = radio_active_ssl_ctx_count;
}

void Radio_GetNetworkBases(void **socket_base, void **amissl_base, void **amissl_master_base)
{
#if defined(AMIGA_M68K)
    if (socket_base) *socket_base = (void *)SocketBase;
#if defined(HAVE_AMISSL)
    if (amissl_base) *amissl_base = (void *)AmiSSLBase;
    if (amissl_master_base) *amissl_master_base = (void *)AmiSSLMasterBase;
#else
    if (amissl_base) *amissl_base = 0;
    if (amissl_master_base) *amissl_master_base = 0;
#endif
#else
    if (socket_base) *socket_base = 0;
    if (amissl_base) *amissl_base = 0;
    if (amissl_master_base) *amissl_master_base = 0;
#endif
}

/* Runtime accessor so radio_stream_probe.c can adopt the shared AmiSSL
 * instance opened here.  The probe file declares its own AmiSSLBase/
 * AmiSSLExtBase/AmiSSLMasterBase as weak symbols expecting the linker to
 * merge them with this file's strong definitions, but the m68k hunk linker
 * demonstrably does NOT merge them (a debug run showed two different
 * AmiSSLBase values alive at once), leaving the probe with its own private
 * NULL copies -- so it re-opened amisslmaster.library and re-ran
 * InitAmiSSLMaster()/OpenAmiSSLTags() in a task that already had the shared
 * instance open, which is exactly the repeated-InitAmiSSLMaster() sequence
 * that raises recoverable alerts and wedges AmiSSL.  Passing the values
 * through a function call instead of relying on symbol merging works with
 * every linker. */
void Radio_GetAmiSslShared(void **amissl_base, void **amissl_ext_base, void **amissl_master_base)
{
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    if (amissl_base) *amissl_base = (void *)AmiSSLBase;
    if (amissl_ext_base) *amissl_ext_base = (void *)AmiSSLExtBase;
    if (amissl_master_base) *amissl_master_base = (void *)AmiSSLMasterBase;
#else
    if (amissl_base) *amissl_base = 0;
    if (amissl_ext_base) *amissl_ext_base = 0;
    if (amissl_master_base) *amissl_master_base = 0;
#endif
}

/* Open bsdsocket.library exactly once, at application startup, instead of
 * once per probe/station/favicon fetch: unlike the per-task AmiSSL context
 * (see radio_ssl_global_init()), bsdsocket.library is a plain shared
 * AmigaOS library with no per-task ownership requirement, so it's safe to
 * keep open for the app's lifetime. AmiSSL itself (amisslmaster.library
 * aside, which was already kept open) stays lazily initialized per-task as
 * before: each probe and each playback child still opens and closes its own
 * per-task AmiSSL context around its own use, because reusing a context
 * opened by a *different* task crashes inside SSL_CTX_new()/SSL_new(). */
void Radio_NetworkInit(void)
{
#if defined(AMIGA_M68K)
    if (!SocketBase) {
        SocketBase = OpenLibrary("bsdsocket.library", 4);
        if (SocketBase) {
            radio_socket_library_open_count++;
            RADIO_DBG(printf("radio-netinit: bsdsocket.library opened socket_library_open_count=%ld\n", radio_socket_library_open_count););
        }
    }
#if defined(HAVE_AMISSL)
    /* Open the shared AmiSSL instance from the main process, per the AmiSSL
     * v5 model: amisslmaster.library + OpenAmiSSLTags() run once here and
     * the returned AmiSSLBase/AmiSSLExtBase are then shared by every probe/
     * playback task, each of which only runs its own per-task
     * InitAmiSSL()/CleanupAmiSSL() pair. Failure is not fatal at startup --
     * HTTPS connects will retry via the lazy fallback and report their own
     * errors. */
    if (SocketBase && radio_amissl_open_shared() != 0)
        RADIO_DBG(printf("radio-netinit: shared AmiSSL open failed (HTTPS unavailable until retry)\n"););
#endif
#endif
}

/* Final network-library teardown at application exit, called only after
 * every playback child has been stopped and reaped.  This is now the only
 * place bsdsocket.library/AmiSSL are closed. */
void Radio_NetworkShutdown(void)
{
#if defined(AMIGA_M68K)
    if (radio_network_shutdown_started) {
        RADIO_DBG(printf("radio-netshutdown: already ran, skipping duplicate shutdown\n"););
        return;
    }
    radio_network_shutdown_started = 1;
#if defined(HAVE_AMISSL)
    if (Radio_IsTlsPoisoned()) {
        /* SSL_free()/SSL_CTX_free()/CleanupAmiSSL() were already deliberately
         * skipped for this run because AmiSSL's internals are suspect -- the
         * final CloseAmiSSL()/CloseLibrary() walk the same corrupted state
         * and were the last remaining source of recoverable alerts on app
         * close after poison. Abandon the whole shared instance instead: the
         * OS reclaims it when the process exits. */
        printf("APP_CLOSE: AmiSSL poisoned, skipping final AmiSSL shutdown\n");
        RADIO_DBG(printf("radio-netshutdown: AmiSSL poisoned (reason=%s), abandoning base=%p ext=%p master=%p without CleanupAmiSSL/CloseAmiSSL/CloseLibrary\n",
            Radio_TlsPoisonReason(), (void *)AmiSSLBase, (void *)AmiSSLExtBase, (void *)AmiSSLMasterBase););
        AmiSSLBase = NULL;
        AmiSSLExtBase = NULL;
        AmiSSLMasterBase = NULL;
        radio_amissl_initialized = 0;
        radio_amissl_task_poisoned = 0;
    } else {
        if (radio_amissl_initialized)
            radio_ssl_global_cleanup();           /* closes any still-open per-task AmiSSL */
        /* AmiSSLBase/AmiSSLExtBase are now opened once for the whole app's
         * lifetime (see radio_amissl_open_shared()) rather than per station
         * switch, so this is the one and only place that closes them. */
        if (AmiSSLBase) {
            RADIO_DBG(printf("radio-netshutdown: closing shared AmiSSL instance base=%p\n", (void *)AmiSSLBase););
            CloseAmiSSL();
            radio_closeamissl_count++;
            AmiSSLBase = NULL;
            AmiSSLExtBase = NULL;
        }
        if (AmiSSLMasterBase) {
            CloseLibrary(AmiSSLMasterBase);
            AmiSSLMasterBase = NULL;
            radio_amisslmaster_close_count++;
            radio_amissl_initialized = 0;
        }
    }
#endif
    if (SocketBase) {
        if (radio_open_socket_count == 0) {
            CloseLibrary(SocketBase);
            SocketBase = NULL;
            radio_socket_library_close_count++;
            RADIO_DBG(printf("radio-netshutdown: bsdsocket.library closed socket_library_close_count=%ld\n", radio_socket_library_close_count););
        } else {
            /* A socket is somehow still open (leaked/late child) -- closing
             * the library out from under it is worse than leaking it for the
             * few ms until process exit. */
            RADIO_DBG(printf("radio-netshutdown: bsdsocket.library left open (unsafe) open_socket_count=%ld\n", radio_open_socket_count););
        }
    }
    RADIO_DBG(printf("radio-netshutdown: final counters socket_library_open_count=%ld socket_library_close_count=%ld amisslmaster_open_count=%ld amisslmaster_close_count=%ld openamissltags_count=%ld closeamissl_count=%ld amissl_init_count=%ld amissl_cleanup_count=%ld active_socket_count=%ld active_ssl_count=%ld active_ssl_ctx_count=%ld tls_poisoned=%d poison_reason=\"%s\"\n",
        radio_socket_library_open_count, radio_socket_library_close_count,
        radio_amisslmaster_open_count, radio_amisslmaster_close_count,
        radio_openamissltags_count, radio_closeamissl_count,
        radio_amissl_init_count, radio_amissl_cleanup_count,
        radio_open_socket_count, radio_active_ssl_count,
        radio_active_ssl_ctx_count, Radio_IsTlsPoisoned(),
        Radio_TlsPoisonReason()););
    RADIO_DBG(printf("radio-netshutdown: tls_fault_count=%ld (fault limit %d)\n",
        radio_tls_fault_count, RADIO_TLS_FAULT_LIMIT););
#endif
}

int Radio_Pump(RadioStream *rs)
{
    unsigned char b[1024];
    int n, wb;
    if (!rs || rs->status == RADIO_STATUS_ERROR) return -1;
    if (radio_is_stopping(rs)) { close_current_socket(rs); rs->status = RADIO_STATUS_CLOSED; return 0; }
    if (rs->sock == RADIO_INVALID_SOCKET) {
        if (!rs->everPlayed) { set_error(rs, "radio stream closed before playback started"); return -1; }
        return reconnect_http(rs);
    }
    wb = 0;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    if (rs->isSSL && rs->ssl) {
        int requested = (int)sizeof(b);
        RADIO_DBG(printf("radio-ssl-read: session=%lu sslHandshakeDone=%d before SSL_read\n", rs->session_id, rs->sslHandshakeDone););
        if (rs->sslHandshakeDone != 1) {
            RADIO_DBG(printf("radio-ssl-read: ERROR session=%lu skipped SSL_read because handshake is incomplete sslHandshakeDone=%d\n", rs->session_id, rs->sslHandshakeDone););
            set_error(rs, "TLS handshake incomplete");
            close_current_socket(rs);
            return -1;
        }
        n = (int)SSL_read(rs->ssl, (char *)b, requested);
        RADIO_DBG(printf("radio-ssl-read: session=%lu ssl=%p ctx=%p fd=%ld dst=%p dst_cap=%d requested=%d returned=%d fill=%lu ring_free=%lu\n",
            rs->session_id, (void *)rs->ssl, (void *)rs->ctx, (long)rs->sock, (void *)b, (int)sizeof(b), requested, n, rs->used, rs->size > rs->used ? rs->size - rs->used : 0));
        if (n <= 0) {
            int e = SSL_get_error(rs->ssl, n);
            RADIO_DBG(printf("radio-ssl-read: session=%lu SSL_get_error=%d ret=%d fd=%ld\n", rs->session_id, e, n, (long)rs->sock));
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) wb = 1;
            else {
                /* Not a "call again later" condition -- a real record-layer
                 * failure (bad MAC, unexpected message, truncated record,
                 * ...). The handshake-failure path already logs the
                 * OpenSSL/AmiSSL error-queue detail behind SSL_get_error()'s
                 * bare numeric code; this path never did, so a read fault
                 * like this had no detail to go on. This SSL object is about
                 * to be torn down via close_current_socket()/SSL_free()
                 * right after this. */
                unsigned long ssl_lib_error = ERR_get_error();
                char ssl_error_buf[160];
                ssl_error_buf[0] = '\0';
                if (ssl_lib_error != 0)
                    ERR_error_string_n(ssl_lib_error, ssl_error_buf, sizeof(ssl_error_buf));
                RADIO_DBG(printf("radio-ssl-read: session=%lu read failed ssl_error=%d lib_error=%08lx (%s)\n",
                    rs->session_id, e, ssl_lib_error, ssl_error_buf[0] ? ssl_error_buf : "none"));
                if (e == SSL_ERROR_SSL || ssl_lib_error != 0) {
                    /* A record-layer failure inside SSL_read(). The old
                     * hard-poison-on-first-fault policy here dated from soak
                     * tests where the following SSL_free()/SSL_CTX_free()
                     * crashed -- but those runs also had two AmiSSL
                     * instances open in one task (the weak-symbol merge
                     * failure fixed in rb_probe_ensure_amissl()), which is
                     * the likelier culprit. Treat this as a normal stream
                     * error now: the session fails cleanly, its SSL objects
                     * are freed normally below, and HTTPS stays enabled.
                     * Radio_ReportTlsFault() keeps a fuse: corruption-grade
                     * repeated faults still hard-poison, and once poisoned
                     * this session's objects are leaked as before. */
                    Radio_ReportTlsFault(e == SSL_ERROR_SSL ? "SSL_ERROR_SSL from SSL_read" : "fatal AmiSSL error queue from SSL_read");
                    ERR_clear_error();
                    if (Radio_IsTlsPoisoned()) {
                        rs->sslStatePoisoned = 1;
                        radio_amissl_task_poisoned = 1;
                        RADIO_DBG(printf("radio-ssl-read: session=%lu SSL state marked poisoned (fault limit reached) -- will skip SSL_free/SSL_CTX_free/CleanupAmiSSL on close\n", rs->session_id));
                    }
                }
            }
        }
    } else
#endif
    {
        n = (int)recv(rs->sock, (char *)b, sizeof(b), 0);
        if (n < 0 && radio_would_block()) wb = 1;
    }
    if (radio_is_stopping(rs)) { close_current_socket(rs); rs->status = RADIO_STATUS_CLOSED; return 0; }
    /* non-blocking socket (or SSL WANT_READ): no data yet — yield */
    if (n < 0 && wb) {
        radio_backoff_sleep();
        if (radio_note_start_wait(rs, rs->isSSL ? "HTTPS stream start timeout" : "radio stream start timed out") < 0) return -1;
        return 0;
    }
    if (n <= 0) {
        close_current_socket(rs);
        if (!rs->headerDone || !rs->firstDataLogged) { rs->decoderStarted = 0; rs->streamStateFlags &= ~DECODER_STARTED; set_error(rs, rs->isSSL ? "HTTPS read failed" : "HTTP header read failed"); RADIO_OPEN_DEBUG_PRINTF(("radio-open: HTTP header failed before ready headerDone=%d firstData=%d\n", rs->headerDone, rs->firstDataLogged)); return -1; }
        if (!rs->everPlayed) { set_error(rs, "radio stream ended before audio buffered"); return -1; }
        rs->reconnectDelay = RADIO_RECONNECT_BACKOFF_PUMPS;
        set_status(rs, RADIO_STATUS_RECONNECTING);
        return 0;
    }
    rs->zeroBytePumps = 0;
    if (!rs->firstDataLogged) {
        RADIO_DBG(printf("radio-stream: first data received fd=%ld ssl=%p\n",
            (long)rs->sock,
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
            (void *)rs->ssl
#else
            (void *)0
#endif
        ));
        rs->firstDataLogged = 1;
    }
    if (!rs->everPlayed) rs->startPumps = 0;
    if (process_bytes(rs, b, n) < 0) return -1;
    if (rs->status == RADIO_STATUS_PLAYING || rs->everPlayed) {
        clock_t now = clock();
        if (!rs->lastMemReportClock) rs->lastMemReportClock = now;
        if ((unsigned long)((now - rs->lastMemReportClock) * 1000UL / CLOCKS_PER_SEC) >= 10000UL) {
            radio_debug_mem_report(rs->session_id, "playing 10s sample");
            rs->lastMemReportClock = now;
        }
    }
    if (radio_is_stopping(rs)) { close_current_socket(rs); rs->status = RADIO_STATUS_CLOSED; return 0; }
    if (rs->headerDone && rs->used >= RADIO_START_THRESHOLD) {
        if (!rs->everPlayed) {
            RADIO_DBG(printf("radio-stream: first decoder frame / playback buffer ready fd=%ld\n", (long)rs->sock);)
            /* Narrows the corruption window seen on session 4 of the SSL/MP3
             * soak test: the ring's own canary passed every ring_write()
             * during buffering (those checks are silent on success), but was
             * found zeroed by the time MP3InitDecoder() ran. This is the
             * exact instant buffering completes, still inside radio_stream.c
             * -- if this check ever fails, the corruption is happening
             * somewhere in this file's own buffering/parse path, not in the
             * decoder/audio-device setup that runs after this point. */
            radio_ring_check_canary(rs, "buffering complete, before decoder handoff");
        }
        rs->reconnectAttempts = 0; rs->reconnectDelay = 0; rs->everPlayed = 1; rs->playbackStarted = 1; rs->streamStateFlags |= PLAYBACK_STARTED; set_status(rs, RADIO_STATUS_PLAYING);
    } else if (rs->headerDone && rs->status != RADIO_STATUS_PLAYING)
        set_status(rs, RADIO_STATUS_BUFFERING);
    return n;
}
int Radio_ReadAudio(RadioStream *rs,unsigned char *buf,int maxBytes){ int got; if(!rs||!buf||maxBytes<=0)return 0; if(radio_is_stopping(rs)) return 0; while(!radio_is_stopping(rs) && rs->status!=RADIO_STATUS_PLAYING && rs->used<RADIO_START_THRESHOLD && rs->status!=RADIO_STATUS_ERROR) { if(Radio_Pump(rs)<=0 && !rs->everPlayed && (++rs->zeroBytePumps>=RADIO_ZERO_BYTE_PUMP_MAX || radio_note_start_wait(rs,"radio stream did not buffer audio")<0)) { if(rs->status!=RADIO_STATUS_ERROR) set_error(rs,"radio stream did not buffer audio"); break; } } while(!radio_is_stopping(rs) && rs->used==0 && rs->status!=RADIO_STATUS_ERROR) { if(Radio_Pump(rs)<=0 && !rs->everPlayed && (++rs->zeroBytePumps>=RADIO_ZERO_BYTE_PUMP_MAX || radio_note_start_wait(rs,"radio stream did not deliver audio")<0)) { if(rs->status!=RADIO_STATUS_ERROR) set_error(rs,"radio stream did not deliver audio"); break; } } if(radio_is_stopping(rs)||!rs->headerDone||!rs->decoderStarted||rs->status==RADIO_STATUS_ERROR) return 0; got=ring_read(rs,buf,maxBytes); if(!rs->everPlayed && rs->status==RADIO_STATUS_PLAYING && rs->used<RADIO_LOW_WATER_BYTES) set_status(rs,RADIO_STATUS_BUFFERING); if(rs->status==RADIO_STATUS_BUFFERING && rs->used>=RADIO_START_THRESHOLD) set_status(rs,RADIO_STATUS_PLAYING); return got; }
int Radio_ReadStartupAudio(RadioStream *rs,unsigned char *buf,int maxBytes,unsigned long timeoutMs){ clock_t start; int got; if(!rs||!buf||maxBytes<=0)return 0; start=clock(); while(!radio_is_stopping(rs)&&rs->used==0&&rs->status!=RADIO_STATUS_ERROR){ if(Radio_Pump(rs)<0)break; if(timeoutMs>0 && (unsigned long)((clock()-start)*1000UL/CLOCKS_PER_SEC)>=timeoutMs){ set_error(rs,"AAC stream start timeout"); close_current_socket(rs); break; } } if(radio_is_stopping(rs)||!rs->headerDone||!rs->decoderStarted||rs->status==RADIO_STATUS_ERROR) return 0; got=ring_read(rs,buf,maxBytes); if(!rs->everPlayed&&rs->headerDone&&rs->status!=RADIO_STATUS_PLAYING&&rs->status!=RADIO_STATUS_ERROR) set_status(rs,RADIO_STATUS_BUFFERING); return got; }
void Radio_FailStartup(RadioStream *rs,const char *message){ if(!rs)return; set_error(rs,message&&message[0]?message:"AAC stream start timeout"); rs->stopping=1; rs->reconnectAttempts=RADIO_RECONNECT_MAX; rs->reconnectDelay=0; close_current_socket(rs); }
RadioStatus Radio_GetStatus(RadioStream *rs){ return rs?rs->status:RADIO_STATUS_CLOSED; }
const char *Radio_GetTitle(RadioStream *rs){ return rs?rs->title:""; }
const char *Radio_GetStationName(RadioStream *rs){ return rs?rs->stationName:""; }
const char *Radio_GetGenre(RadioStream *rs){ return rs?rs->genre:""; }
const char *Radio_GetStreamUrl(RadioStream *rs){ return rs?rs->streamUrl:""; }
int Radio_GetMetaInt(RadioStream *rs){ return rs?rs->metaint:0; }
const char *Radio_GetContentType(RadioStream *rs){ return rs?rs->contentType:""; }
const char *Radio_GetError(RadioStream *rs){ return rs?(rs->error[0]?rs->error:""):"radio not open"; }
int Radio_GetBitrate(RadioStream *rs){ return rs?rs->bitrate:0; }
int Radio_GetBufferedBytes(RadioStream *rs){ return rs?(int)rs->used:0; }
const char *Radio_StatusText(RadioStatus s){ switch(s){case RADIO_STATUS_CONNECTING:return "Connecting";case RADIO_STATUS_BUFFERING:return "Buffering";case RADIO_STATUS_PLAYING:return "Playing";case RADIO_STATUS_RECONNECTING:return "Reconnecting";case RADIO_STATUS_STOPPING:return "Stopping";case RADIO_STATUS_CLOSED:return "Closed";case RADIO_STATUS_ERROR:return "Error";default:return "Idle";} }

#endif /* ENABLE_RADIO */
