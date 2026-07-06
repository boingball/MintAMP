#ifndef ENABLE_RADIO
#define ENABLE_RADIO 0
#endif
#if ENABLE_RADIO
#include "radio_stream.h"
#include "amiga_display_text.h"
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
/* Mid-stream stall watchdog: consecutive would-block pumps (each preceded by
 * radio_backoff_sleep()'s ~40ms yield, so 750 is roughly 30 seconds) with no
 * data after playback has started.  A dead relay that keeps the TCP session
 * open but never sends another byte (and never a FIN) otherwise pumps forever:
 * the startup timeout (RADIO_START_TIMEOUT_PUMPS) only arms before everPlayed,
 * so the stream sat in "Buffering" for good.  On expiry the stream fails with
 * set_error() exactly like the startup timeout, so the child unwinds and
 * exits; see the comment at the check itself for why this is not an
 * automatic reconnect. */
#ifndef RADIO_STALL_TIMEOUT_PUMPS
#define RADIO_STALL_TIMEOUT_PUMPS 750
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
#include <exec/memory.h>
#include <exec/execbase.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <exec/semaphores.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/bsdsocket.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
extern struct ExecBase *SysBase;
struct Library *SocketBase = NULL;
/* Single-worker-task architecture (HAVE_AMISSL builds): every read/write of
 * AmiSSLBase/AmiSSLExtBase/AmiSSLMasterBase/SocketBase now happens only on
 * the one long-lived radio net worker task started by
 * radio_net_worker_ensure_started() -- see the "single AmiSSL/bsdsocket
 * worker" block below. Because exactly one task ever touches them, there is
 * no more cross-task race to guard: Radio_AmiSslLock()/Unlock() are kept as
 * trivial no-ops purely for API compatibility with radio_stream_probe.c. */
int Radio_AmiSslLock(void) { return 1; }
void Radio_AmiSslUnlock(void) { }
#define RADIO_SOCKET long
#define RADIO_INVALID_SOCKET (-1)
#define radio_close_socket(s) CloseSocket(s)
#if defined(HAVE_AMISSL)
#include <libraries/amisslmaster.h>
#include <proto/amisslmaster.h>
#include <proto/amissl.h>
#include <amissl/amissl.h>
#include <errno.h>
/* Strong definitions, private to the radio net worker task -- see the
 * "single AmiSSL/bsdsocket worker" block below.  radio_stream_probe.c still
 * references these same symbol names (required by the AmiSSL/bsdsocket
 * proto-header stubs, which resolve calls through a global of this exact
 * name) but only ever executes on this same worker task, via
 * Radio_RunOnNetWorker(); radio_browser_http.c and every other (GUI/opener)
 * task never see a non-NULL value from Radio_GetNetworkBases()/
 * Radio_GetAmiSslShared() and always fall back to opening their own
 * plain bsdsocket.library base instead. */
struct Library *AmiSSLMasterBase = NULL;
struct Library *AmiSSLBase = NULL;
struct Library *AmiSSLExtBase = NULL;
static int radio_amissl_initialized = 0;
/* Set alongside RadioStream::sslStatePoisoned (see Radio_Pump()'s
 * SSL_ERROR_SSL handling) -- mirrors it at file scope so the worker's own
 * per-task AmiSSL cleanup can be skipped too. One worker task only ever
 * touches AmiSSL, so a single flag is enough. */
static int radio_amissl_task_poisoned = 0;
/* Reason slug from whichever session most recently set radio_amissl_task_poisoned,
 * so the worker's CleanupAmiSSL skip can log why, even though that state is
 * task-global and no longer has the RadioStream. */
static char radio_amissl_task_poison_reason[24];

/* bsdsocket.library and AmiSSL are opened once by the net worker task at
 * startup and closed once when it exits -- these count those two events
 * specifically (expected to reach 1 each over an entire run), as opposed to
 * the per-connection socket/SSL/SSL_CTX counts declared further down, which
 * are expected to churn constantly. Declared up here because the worker
 * entry point below (radio_net_worker_entry) increments them at file-scope
 * before any per-connection counters are declared. */
static long radio_socket_library_open_count = 0;
static long radio_socket_library_close_count = 0;
static long radio_amissl_init_count = 0;
static long radio_amissl_cleanup_count = 0;
static long radio_amisslmaster_open_count = 0;
static long radio_amisslmaster_close_count = 0;
static long radio_openamissltags_count = 0;
static long radio_closeamissl_count = 0;

/* ------------------------------------------------------------------------
 * Single long-lived AmiSSL/bsdsocket worker task.
 *
 * Mirrors amissl_child_worker_repro.c exactly: one persistent task opens
 * bsdsocket.library + amisslmaster.library + OpenAmiSSLTags() (with
 * AmiSSL_InitAmiSSL, TRUE and its own private AmiSSL_ErrNoPtr storage,
 * radio_net_worker_errno_store -- never the shared/global errno) exactly
 * once for the whole app run, then owns the pump loop for every active
 * station. Station open/close requests are still dispatched to that task,
 * but steady-state reads happen autonomously there (not as per-pump RPCs),
 * and each station only creates/frees a per-connection SSL/SSL_CTX/socket,
 * never touching the library bases again -- before
 * finally closing everything exactly once, in the same order the repro
 * uses, when the app asks it to shut down.
 *
 * The GUI/opener task and radio_stream_probe.c never read or write
 * SocketBase/AmiSSLBase/AmiSSLExtBase/AmiSSLMasterBase/the worker's errno
 * store directly -- they hand a small closure to Radio_RunOnNetWorker(),
 * which either runs it immediately (already on the worker task -- e.g.
 * nested calls from within a job) or ships it to the worker task over an
 * AmigaOS message port and blocks (with a generous but bounded timeout,
 * consistent with this file's existing "never wait on a foreign task
 * forever" philosophy -- see the old Radio_AmiSslLock() comment this
 * replaces) until the worker replies. */
static struct Task *radio_net_worker_task = NULL;
static struct MsgPort *radio_net_worker_port = NULL;
static volatile int radio_net_worker_ready = 0;     /* worker finished start-up (libs open or not) */
static volatile int radio_net_worker_libs_ok = 0;   /* worker's bsdsocket.library open succeeded (message loop is running) */
static volatile int radio_net_worker_https_ok = 0;  /* worker's AmiSSL instance also opened -- HTTPS available */
static long radio_net_worker_errno_store = 0;       /* worker task's own AmiSSL_ErrNoPtr storage */
static RadioStream *radio_net_worker_streams = NULL;  /* worker-owned list of streams to pump autonomously */
static int radio_net_worker_pump_active = 0;          /* reentrancy guard for the autonomous pump loop */

static int radio_pump_body(RadioStream *rs);
static void radio_worker_pump_active_streams(void);
static void radio_stream_lock(RadioStream *rs);
static void radio_stream_unlock(RadioStream *rs);

/* ~60s at ~40ms/poll: generous enough to cover connect_http()'s worst-case
 * DNS + TCP connect + TLS handshake budget end to end, while still bounded
 * -- a caller must never Wait() forever on a worker that might be wedged
 * inside a blocking bsdsocket/AmiSSL call (this file's history has already
 * seen CleanupAmiSSL() loop forever on corrupted internals). */
#define RADIO_NET_WORKER_WAIT_TRIES 1500
/* ~10s: bounded wait for the worker task itself to start up and finish
 * opening its libraries. */
#define RADIO_NET_WORKER_START_TRIES 250

typedef struct RadioNetWorkerJob {
    struct Message msg;
    void (*fn)(void *arg);
    void *arg;
    int isShutdown;
} RadioNetWorkerJob;

static int radio_net_worker_is_self(void)
{
    return radio_net_worker_task != NULL && FindTask(NULL) == radio_net_worker_task;
}

/* True when called BY the worker task itself -- the only task that ever
 * runs OpenAmiSSLTags()/InitAmiSSL() in this process, so it is always "the
 * opener" per the AmiSSL v5/v6 SDK and must never run a manual
 * InitAmiSSL()/CleanupAmiSSL() pair on top of that. */
int Radio_AmiSslTaskIsOpener(void) { return radio_net_worker_is_self(); }

static void radio_net_worker_entry(void)
{
    struct MsgPort *port;
    int shuttingDown = 0;

    RADIO_DBG(printf("radio-net-worker: starting task=%p\n", (void *)FindTask(NULL)););

    SocketBase = OpenLibrary("bsdsocket.library", 4);
    if (SocketBase) radio_socket_library_open_count++;
    if (SocketBase)
        AmiSSLMasterBase = OpenLibrary("amisslmaster.library", AMISSLMASTER_MIN_VERSION);
    if (AmiSSLMasterBase) radio_amisslmaster_open_count++;
    if (SocketBase && AmiSSLMasterBase) {
        if (OpenAmiSSLTags(AMISSL_CURRENT_VERSION,
                           AmiSSL_UsesOpenSSLStructs, TRUE,
                           AmiSSL_InitAmiSSL, TRUE,
                           AmiSSL_GetAmiSSLBase, (ULONG)&AmiSSLBase,
                           AmiSSL_GetAmiSSLExtBase, (ULONG)&AmiSSLExtBase,
                           AmiSSL_SocketBase, (ULONG)SocketBase,
                           AmiSSL_ErrNoPtr, (ULONG)&radio_net_worker_errno_store,
                           TAG_DONE) != 0) {
            AmiSSLBase = NULL;
            AmiSSLExtBase = NULL;
        } else {
            radio_openamissltags_count++;
            radio_amissl_init_count++;
        }
    }
    radio_amissl_initialized = (AmiSSLBase != NULL);
    radio_net_worker_libs_ok = (SocketBase != NULL) ? 1 : 0;
    radio_net_worker_https_ok = (AmiSSLBase != NULL) ? 1 : 0;
    RADIO_DBG(printf("radio-net-worker: libs_ok=%d https_ok=%d SocketBase=%p AmiSSLMasterBase=%p AmiSSLBase=%p AmiSSLExtBase=%p ErrNoPtr=%p\n",
        radio_net_worker_libs_ok, radio_net_worker_https_ok, (void *)SocketBase, (void *)AmiSSLMasterBase,
        (void *)AmiSSLBase, (void *)AmiSSLExtBase, (void *)&radio_net_worker_errno_store););

    port = radio_net_worker_libs_ok ? CreateMsgPort() : NULL;
    radio_net_worker_port = port;
    radio_net_worker_ready = 1; /* publish last: port + libs_ok are now safe to read */

    if (port) {
        for (;;) {
            RadioNetWorkerJob *job;
            while ((job = (RadioNetWorkerJob *)GetMsg(port)) != NULL) {
                if (job->isShutdown)
                    shuttingDown = 1;
                else if (job->fn)
                    job->fn(job->arg);
                ReplyMsg(&job->msg);
            }
            if (shuttingDown) break;
            radio_worker_pump_active_streams();
            Delay(2);
        }
    }

    RADIO_DBG(printf("radio-net-worker: shutting down task=%p\n", (void *)FindTask(NULL)););
    if (AmiSSLBase) { CloseAmiSSL(); AmiSSLBase = NULL; AmiSSLExtBase = NULL; }
    if (AmiSSLMasterBase) { CloseLibrary(AmiSSLMasterBase); AmiSSLMasterBase = NULL; }
    if (SocketBase) { CloseLibrary(SocketBase); SocketBase = NULL; }
    radio_amissl_initialized = 0;
    if (port) DeleteMsgPort(port);
    radio_net_worker_port = NULL;
    radio_net_worker_ready = 0;
    radio_net_worker_libs_ok = 0;
    radio_net_worker_https_ok = 0;
    /* radio_net_worker_task itself is left as-is: Radio_NetworkShutdown()
     * (the only caller that gets here) is about to exit the whole app, and
     * clearing it here would race the exiting worker task with any other
     * task still holding a (now stale) reference. */
}

/* Lazily start the worker (safe to call repeatedly -- Radio_NetworkInit()
 * and the first station open both run on the GUI/opener task, so there is
 * no concurrent-start race to guard) and wait, bounded, for it to finish
 * opening its libraries. Returns 1 if the worker is up and its libraries
 * opened, 0 otherwise (never started, or its own OpenAmiSSLTags()/
 * OpenLibrary() failed). */
static int radio_net_worker_ensure_started(void)
{
    int tries;
    if (radio_net_worker_ready) return radio_net_worker_libs_ok;
    if (!radio_net_worker_task) {
        struct Process *proc = CreateNewProcTags(
            NP_Entry, (ULONG)radio_net_worker_entry,
            NP_Name, (ULONG)"MiniAMP3 radio net worker",
            NP_Priority, 0,
            NP_StackSize, 131072,
            NP_CopyVars, FALSE,
            TAG_DONE);
        if (!proc) return 0;
        radio_net_worker_task = (struct Task *)proc;
    }
    for (tries = 0; tries < RADIO_NET_WORKER_START_TRIES && !radio_net_worker_ready; tries++)
        Delay(2);
    return radio_net_worker_ready ? radio_net_worker_libs_ok : 0;
}

/* Run fn(arg) synchronously, either directly (already on the worker task --
 * e.g. a job calling back into another helper) or by shipping it to the
 * worker task over its message port and blocking for the reply. This is the
 * ONLY way any code in this program may touch bsdsocket.library/AmiSSL: no
 * other task ever calls socket()/connect()/SSL_*()/InitAmiSSL() itself.
 * Returns 1 if fn ran (on whichever task), 0 if the worker could not be
 * started or the wait timed out. On a timeout, job/replyPort are
 * deliberately leaked rather than freed -- the worker may still be about to
 * write its reply into them -- same "leak rather than risk a crash"
 * trade-off this file already makes for a wedged CleanupAmiSSL(). */
int Radio_RunOnNetWorker(void (*fn)(void *arg), void *arg)
{
    RadioNetWorkerJob *job;
    struct MsgPort *replyPort;
    int tries;

    if (!fn) return 0;
    if (radio_net_worker_is_self()) { fn(arg); return 1; }
    if (!radio_net_worker_ensure_started()) return 0;

    job = (RadioNetWorkerJob *)malloc(sizeof(*job));
    if (!job) return 0;
    replyPort = CreateMsgPort();
    if (!replyPort) { free(job); return 0; }

    memset(&job->msg, 0, sizeof(job->msg));
    job->msg.mn_ReplyPort = replyPort;
    job->msg.mn_Length = sizeof(*job);
    job->fn = fn;
    job->arg = arg;
    job->isShutdown = 0;
    PutMsg(radio_net_worker_port, &job->msg);

    for (tries = 0; tries < RADIO_NET_WORKER_WAIT_TRIES; tries++) {
        if (GetMsg(replyPort)) {
            DeleteMsgPort(replyPort);
            free(job);
            return 1;
        }
        Delay(2);
    }
    RADIO_DBG(printf("radio-net-worker: job dispatch timed out, worker may be wedged -- leaking job=%p replyPort=%p\n", (void *)job, (void *)replyPort););
    return 0;
}

/* Ask the worker task to close its libraries and exit, then wait, bounded,
 * for it to actually do so. Only called once, from Radio_NetworkShutdown(). */
static int radio_net_worker_stop(void)
{
    RadioNetWorkerJob job;
    struct MsgPort *replyPort;
    int tries;

    if (!radio_net_worker_ready || !radio_net_worker_port) return 1; /* never started, or already gone */

    replyPort = CreateMsgPort();
    if (!replyPort) return 0;
    memset(&job.msg, 0, sizeof(job.msg));
    job.msg.mn_ReplyPort = replyPort;
    job.msg.mn_Length = sizeof(job);
    job.fn = NULL;
    job.arg = NULL;
    job.isShutdown = 1;
    PutMsg(radio_net_worker_port, &job.msg);

    for (tries = 0; tries < RADIO_NET_WORKER_WAIT_TRIES; tries++) {
        if (GetMsg(replyPort)) break;
        Delay(2);
    }
    DeleteMsgPort(replyPort);
    /* Give the worker a further bounded moment to finish CloseAmiSSL()/
     * CloseLibrary() and clear radio_net_worker_ready after replying. */
    for (tries = 0; tries < RADIO_NET_WORKER_START_TRIES && radio_net_worker_ready; tries++)
        Delay(2);
    return !radio_net_worker_ready;
}
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
int Radio_AmiSslLock(void) { return 0; }
void Radio_AmiSslUnlock(void) { }
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
    /* Sized to match the probe's RB_PROBE_MAX_* (512/256/512): the old
     * url[256]/host[128]/path[192] silently truncated long tokenized stream
     * paths -- f121.rndfnk.com's 194-char path lost the end of its auth
     * token, the server answered the mangled request with data AmiSSL's
     * record parser could not survive, and playback died with "invalid
     * record" while the (512-byte) probe of the same URL worked fine. */
    char url[512], host[256], path[512];
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
    int stallPumps;   /* consecutive would-block pumps since last data, mid-stream */
    int everPlayed;
    int firstDataLogged;
    int firstMetaLogged;
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
    /* Which task called Radio_OpenWithHostAddr()/owns this session's
     * Radio_Pump()/Radio_ReadAudio() calls -- the single net worker task
     * peeks at this task's own pending SIGBREAKF_CTRL_C (radio_is_stopping())
     * so a GUI front end that signals this task directly (as both front
     * ends already do to interrupt playback) still aborts a connect/
     * handshake/read the worker is running on this session's behalf. */
    struct Task *requestingTask;
    SSL *ssl;
    SSL_CTX *ctx;
    int sslHandshakeDone;
    int sslStatePoisoned; /* see radio_ssl_read()'s SSL_ERROR_SSL handling */
    int tlsFaultCounted;  /* this session already counted toward the fault fuse */
    int lastSslError;     /* last SSL_get_error() value that set sslStatePoisoned, for pre-SSL_free diagnostics */
    char lastSslOp[24];   /* reason slug ("ssl-read-syscall", etc.) that set sslStatePoisoned */
    /* SSL_ERROR_ZERO_RETURN (a clean TLS close_notify) is NOT a fatal fault --
     * sslStatePoisoned/HTTPS-wide quarantine must not fire for it -- but
     * radio_stream_probe.c's rb_probe_ssl_read_retrying() found AmiSSL can
     * still leave SSL_free() unsafe after this specific "clean" close.
     * Mirror its narrowly-scoped sslReadCloseSeen flag here: skip SSL_free
     * for *this object only*, with no task-wide poisoning and no HTTPS
     * disablement. */
    int sslReadCloseSeen;
    struct SignalSemaphore workerLock;
    struct RadioStream *workerNext;
    int workerRegistered;
    int workerClosedAck;
    int workerDetached;
#endif
    /* Set once a read/write/connect fault is classified fatal (see
     * radio_ssl_error_is_fatal()): this session is done, permanently. No
     * further pumping, no reconnect, no AAC-timeout wait -- Radio_Pump() and
     * reconnect_http() both refuse to do anything once these are set. */
    int fatalStop;
    int noReconnect;
    /* radio_ssl_close_stream_mode()/close_current_socket() can be re-entered
     * (abort path, then Radio_Close()'s own call); once a session has fully
     * closed its socket and freed/quarantined SSL, repeat calls must be a
     * cheap no-op instead of re-running (and re-logging) the whole sequence. */
    int closeCleanupDone;
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

/* Radio_NetworkShutdown() must be idempotent: the app-close path has a
 * fallback call site, and running the AmiSSL/bsdsocket teardown twice is
 * exactly the kind of double-cleanup this file's counters exist to catch. */
static int radio_network_shutdown_started = 0;

static void radio_copy_string(char *dst, size_t dstSize, const char *src);

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

/* Debug-only escape hatch (MP3_PANIC_EXIT_ON_MEM_CORRUPT=1 in the
 * environment): on detected corruption, skip every further cleanup attempt
 * -- tidy or otherwise -- and terminate this task/process immediately. Any
 * further tidy cleanup (freeing the ring, the RadioStream, the decoder,
 * disposing GUI objects, closing shared libraries) is itself a write through
 * the same allocator/library state that triggered this, so for soak-testing
 * whether cleanup-after-corruption is what turns a detected fault into a
 * recoverable alert, this mode removes cleanup from the equation entirely. */
static int radio_panic_exit_on_mem_corrupt(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("MP3_PANIC_EXIT_ON_MEM_CORRUPT");
        cached = (v && *v && *v != '0') ? 1 : 0;
    }
    return cached;
}

void Radio_MarkMemoryPoisoned(const char *where)
{
    radioMemoryPoisoned = 1;
    printf("radio-memory: Memory corruption detected - restart app (where=%s session=%lu url=\"%s\")\n",
        where ? where : "", radio_poison_session_id,
        radio_poison_url[0] ? radio_poison_url : "");
    /* A corrupt heap must also hard-quarantine AmiSSL (audit rule 9): TLS
     * poison blocks every later HTTPS probe/playback this run, makes each
     * task's cleanup skip CleanupAmiSSL(), and makes Radio_NetworkShutdown()
     * abandon the instance instead of walking CleanupAmiSSL()/CloseAmiSSL()/
     * CloseLibrary() through damaged memory. Previously only the ring-canary
     * path set this, so a MiniMem report without a TLS fault still cleaned
     * AmiSSL up on a known-corrupt heap. */
    Radio_MarkTlsPoisoned(where && where[0] ? where : "MiniMem heap corruption");
    if (radio_panic_exit_on_mem_corrupt()) {
        printf("radio-memory: MP3_PANIC_EXIT_ON_MEM_CORRUPT active -- exiting immediately without cleanup (where=%s)\n",
            where ? where : "");
        fflush(stdout);
        exit(20);
    }
}

int Radio_IsMemoryPoisoned(void)
{
    return radioMemoryPoisoned;
}

const char *Radio_TlsPoisonedMessage(void)
{
    /* Only detected memory corruption reaches this state now (TLS faults
     * quarantine and continue). Reboot, not just app restart: once poisoned
     * cleanup has been skipped, the corrupted amissl.library stays resident
     * with a nonzero open count (AmigaOS never reclaims library opens from
     * an exited process), so a relaunch gets handed the same broken
     * library. */
    return "HTTPS disabled after memory corruption; reboot before using HTTPS.";
}

/* Fatal SSL_read faults are survivable: the faulting session's SSL/SSL_CTX
 * are quarantined (leaked, never freed -- an f121.rndfnk.com run proved
 * SSL_free() after "0A000126 unexpected eof" corrupts the exec heap:
 * AN_BadFreeAddr mid-SSL_free, then a deadend AN_MemCorrupt) and the
 * faulting task skips its CleanupAmiSSL(), but HTTPS itself stays enabled
 * for fresh sessions with fresh objects -- always. There is deliberately no
 * fault-count fuse any more: each quarantined fault costs one leaked
 * SSL/SSL_CTX (~100-150KB) and nothing else, whereas disabling HTTPS costs
 * the user the rest of their session, so only *detected memory corruption*
 * (ring canary / MiniMem check failures) hard-poisons now. */
static long radio_tls_fault_count = 0;
/* Once any SSL_free/SSL_CTX_free/CleanupAmiSSL has been skipped this run,
 * dead tasks have left dangling per-task state inside AmiSSL, so the final
 * CloseAmiSSL() walk at app exit is no longer safe -- Radio_NetworkShutdown()
 * abandons the instance instead (same as hard poison, but HTTPS keeps
 * working until then). */
static int radio_tls_shutdown_quarantine = 0;

void Radio_SetTlsFaultContext(unsigned long session_id, const char *url)
{
    radio_poison_session_id = session_id;
    radio_copy_string(radio_poison_url, sizeof(radio_poison_url), url ? url : "");
}

void Radio_ReportTlsFault(const char *where)
{
    radio_tls_fault_count++;
    radio_tls_shutdown_quarantine = 1;
    RADIO_DBG(printf("radio-tls: TLS fault %ld where=%s session=%lu url=\"%s\" -- objects quarantined, HTTPS stays enabled\n",
        radio_tls_fault_count, where ? where : "",
        radio_poison_session_id, radio_poison_url[0] ? radio_poison_url : ""));
    (void)where;
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
    radio_tls_shutdown_quarantine = 1;
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

/* Proactive exec-heap validation (debug builds): walks every MemHeader's
 * free-chunk list under Forbid() checking the same invariants exec's own
 * FreeMem() trips AN_MemCorrupt (81000005) on -- chunk inside its header's
 * bounds, aligned, sanely sized, strictly ascending, free-byte total
 * matching mh_Free. A TLS-clean run still died with AN_MemCorrupt inside a
 * healthy SSL_free() after three perfect play/stop cycles, so something
 * corrupts exec memory silently during NORMAL operation and is only
 * discovered ages later; running this at every session boundary brackets
 * the corruptor to one interval in the log instead. Read-only, so it is
 * safe to call anywhere; costs one list walk (a few hundred nodes). */
void Radio_DebugCheckExecMem(const char *where)
{
#if defined(AMIGA_M68K) && defined(RADIO_DEBUG)
    struct MemHeader *mh;
    long headers = 0;
    long chunks = 0;
    unsigned long total_free = 0;
    const char *fault = NULL;
    void *fault_addr = NULL;
    Forbid();
    for (mh = (struct MemHeader *)((struct ExecBase *)SysBase)->MemList.lh_Head;
         mh->mh_Node.ln_Succ && !fault;
         mh = (struct MemHeader *)mh->mh_Node.ln_Succ) {
        struct MemChunk *mc = mh->mh_First;
        unsigned long header_free = 0;
        long guard = 0;
        headers++;
        while (mc) {
            if ((void *)mc < mh->mh_Lower || (void *)mc >= mh->mh_Upper) { fault = "chunk outside header"; fault_addr = (void *)mc; break; }
            if (((unsigned long)mc) & 3UL) { fault = "misaligned chunk"; fault_addr = (void *)mc; break; }
            if (mc->mc_Bytes == 0 || (unsigned long)mc + mc->mc_Bytes > (unsigned long)mh->mh_Upper) { fault = "bad chunk size"; fault_addr = (void *)mc; break; }
            if (mc->mc_Next && mc->mc_Next <= mc) { fault = "non-ascending chunk"; fault_addr = (void *)mc; break; }
            header_free += mc->mc_Bytes;
            chunks++;
            if (++guard > 65536) { fault = "chunk list loop"; fault_addr = (void *)mc; break; }
            mc = mc->mc_Next;
        }
        if (!fault && header_free != mh->mh_Free) { fault = "mh_Free mismatch"; fault_addr = (void *)mh; }
        total_free += header_free;
    }
    Permit();
    if (fault) {
        printf("radio-memcheck: CORRUPT %s at %p where=%s (exec heap damaged BEFORE this point)\n",
            fault, fault_addr, where ? where : "");
        fflush(stdout);
        /* This walk previously only printed: a real exec-heap corruption hit
         * in production here (a "bad chunk size" fault) and the app kept
         * probing/reconnecting/starting new playback children on a heap
         * already known to be damaged. Radio_MarkMemoryPoisoned() sets the
         * same radioMemoryPoisoned flag every probe entry point
         * (rb_probe_stream_url_impl/rb_probe_fetch_binary_impl) and playback
         * entry point (RadioDoProbeAndPlay/StartPlayback in minimp3r.c)
         * already checks before doing any DNS/socket/SSL/CreateNewProc work
         * -- it also hard-poisons TLS (Radio_MarkTlsPoisoned(), called
         * internally) so no further SSL_new() runs either. */
        Radio_MarkMemoryPoisoned(where);
    } else {
        printf("radio-memcheck: OK where=%s headers=%ld chunks=%ld free=%lu\n",
            where ? where : "", headers, chunks, total_free);
        fflush(stdout);
    }
#else
    (void)where;
#endif
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

/* App-side stop flag (Radio_SetStopFlag).  Polled by radio_is_stopping() so
 * the pump/connect/read-audio wait loops notice a Stop click even while
 * stalled on a dead socket that never delivers another byte -- the only
 * other stop propagation path (Radio_RequestStop() from InputSourceClose()/
 * InputSourceRead()) runs between Radio_ReadAudio() calls, which a stalled
 * buffering loop never returns to.
 *
 * Deliberately a pointer to a data flag, NOT a callback: polling means only
 * data reads through the pointer, and a data read cannot raise the
 * odd-address instruction-fetch fault (guru 80000003) that calling through
 * a corrupted function pointer can.  Both GUI front-ends set their shared
 * interrupt flag before signalling the playback child, so reading the flag
 * is sufficient; the pending-CTRL_C case is handled below with a direct,
 * statically linked SetSignal() call instead of app code. */
static const volatile int *radio_external_stop_flag = 0;

void Radio_SetStopFlag(const volatile int *flag) { radio_external_stop_flag = flag; }

static int radio_is_stopping(const RadioStream *rs)
{
    if (!rs || rs->stopping || rs->status == RADIO_STATUS_STOPPING || rs->status == RADIO_STATUS_CLOSED)
        return 1;
    if (radio_external_stop_flag && *radio_external_stop_flag)
        return 1;
#if defined(AMIGA_M68K)
    /* A pending break on the pumping task (the GUIs signal their playback
     * child with SIGBREAKF_CTRL_C; a CLI run gets it from the shell).
     * SetSignal(0,0) only samples -- the signal stays pending for the
     * existing break checks in the decoder loops to latch as before. */
    if (SetSignal(0, 0) & SIGBREAKF_CTRL_C)
        return 1;
#if defined(HAVE_AMISSL)
    /* This connect/handshake/read loop may actually be running on the net
     * worker task (see Radio_RunOnNetWorker()), a different task from the
     * one both GUI front ends signal directly to interrupt playback
     * (SignalPlaybackChildCtrlC() et al target the playback child by name/
     * Task pointer, unaware the worker task even exists). Peek at that
     * requesting task's own pending SIGBREAKF_CTRL_C under Forbid() -- the
     * same safe, well-known technique those front ends already use to find
     * the task in the first place -- so a Stop click still aborts a
     * connect/handshake/read the worker is running on this session's
     * behalf. */
    if (rs->requestingTask && rs->requestingTask != (struct Task *)FindTask(NULL)) {
        ULONG pending;
        Forbid();
        pending = rs->requestingTask->tc_SigRecvd;
        Permit();
        if (pending & SIGBREAKF_CTRL_C)
            return 1;
    }
#endif
#endif
    return 0;
}
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
    rs->tlsFaultCounted = 0;
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
    rs->stallPumps = 0;
    rs->everPlayed = rs->firstDataLogged = rs->haveHostAddr = 0;
    rs->sslFreed = rs->ctxFreed = rs->socketClosed = rs->cleanupDone = 0;
    rs->closeCleanupDone = 0;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    InitSemaphore(&rs->workerLock);
    rs->workerNext = NULL;
    rs->workerRegistered = 0;
    rs->workerClosedAck = 0;
    rs->workerDetached = 0;
#endif
}


#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
static void radio_stream_lock(RadioStream *rs)
{
    if (rs) ObtainSemaphore(&rs->workerLock);
}

static void radio_stream_unlock(RadioStream *rs)
{
    if (rs) ReleaseSemaphore(&rs->workerLock);
}
#else
static void radio_stream_lock(RadioStream *rs) { (void)rs; }
static void radio_stream_unlock(RadioStream *rs) { (void)rs; }
#endif

#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
static void radio_worker_register_stream(RadioStream *rs)
{
    if (!rs || rs->workerRegistered) return;
    rs->workerNext = radio_net_worker_streams;
    radio_net_worker_streams = rs;
    rs->workerRegistered = 1;
    RADIO_DBG(printf("radio-net-worker: registered pump stream session=%lu\n", rs->session_id););
}

static void radio_worker_unregister_stream(RadioStream *rs)
{
    RadioStream **pp;
    if (!rs || !rs->workerRegistered) return;
    pp = &radio_net_worker_streams;
    while (*pp) {
        if (*pp == rs) {
            *pp = rs->workerNext;
            radio_stream_lock(rs);
            rs->workerNext = NULL;
            rs->workerRegistered = 0;
            rs->workerClosedAck = 1;
            radio_stream_unlock(rs);
            RADIO_DBG(printf("radio-net-worker: unregistered pump stream session=%lu\n", rs->session_id););
            return;
        }
        pp = &(*pp)->workerNext;
    }
    radio_stream_lock(rs);
    rs->workerNext = NULL;
    rs->workerRegistered = 0;
    rs->workerClosedAck = 1;
    radio_stream_unlock(rs);
}

static void radio_worker_unregister_stream_job(void *arg)
{
    radio_worker_unregister_stream((RadioStream *)arg);
}

static void radio_worker_pump_active_streams(void)
{
    RadioStream *rs;
    if (radio_net_worker_pump_active) return;
    radio_net_worker_pump_active = 1;
    rs = radio_net_worker_streams;
    while (rs) {
        RadioStream *next = rs->workerNext;
        if (rs->status == RADIO_STATUS_ERROR || rs->status == RADIO_STATUS_CLOSED)
            radio_worker_unregister_stream(rs);
        else if (rs->used < rs->size) {
            int budget;
            for (budget = 0; budget < 8 && rs->used < rs->size; budget++) {
                if (radio_pump_body(rs) <= 0)
                    break;
                if (rs->status == RADIO_STATUS_ERROR || rs->status == RADIO_STATUS_CLOSED)
                    break;
            }
        }
        rs = next;
    }
    radio_net_worker_pump_active = 0;
}
#endif

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
/* radio_net_adopt_context()/RadioNetContext (the old "each playback child
 * opens its own bsdsocket.library base and swaps the shared SocketBase
 * global to point at it") are gone: the single net worker task above owns
 * SocketBase/AmiSSLBase/AmiSSLExtBase/AmiSSLMasterBase for the whole app
 * run and every socket()/connect()/SSL_*() call in this file now only ever
 * runs on that one task (via Radio_RunOnNetWorker()), so there is nothing
 * left to adopt or swap. Kept as a no-op purely so the many existing call
 * sites below (radio_wait_connected(), radio_send_all(), connect_http(),
 * radio_ssl_do_handshake(), radio_ssl_connect(), radio_ssl_close_stream_mode(),
 * radio_ssl_free_ctx(), radio_abort_current_socket(), close_current_socket_mode())
 * do not all need editing. */
static void radio_net_adopt_context(RadioStream *rs) { (void)rs; }

/* No more per-child InitAmiSSL()/CleanupAmiSSL(): the worker task ran
 * OpenAmiSSLTags()/InitAmiSSL() exactly once at start-up (see
 * radio_net_worker_entry() above) and stays initialised for the app's whole
 * run, across every station switch. connect_http()/radio_ssl_connect() just
 * need to confirm the worker's AmiSSL instance actually came up. */
static int radio_net_worker_amissl_ready(RadioStream *rs)
{
    if (Radio_IsTlsPoisoned()) {
        /* AmiSSL cleanup has already been skipped/leaked for this run --
         * never call back into it. */
        set_error(rs, Radio_TlsPoisonedMessage());
        return -1;
    }
    if (!AmiSSLBase || !SocketBase) {
        set_error(rs, "AmiSSL unavailable: network worker not running");
        return -1;
    }
    return 0;
}

/* Kept for API compatibility (radio_browser_http.c calls this to find out
 * whether it is safe to open its own independent bsdsocket.library base
 * right now).  With the single-worker architecture every subsystem opens
 * its own private bsdsocket base and none of them ever touch the worker's
 * SocketBase, so there is no more contention to report. */
int Radio_PlaybackOwnsNetwork(void) { return radio_net_worker_streams != NULL; }

/* No per-child InitAmiSSL()/CleanupAmiSSL()/bsdsocket.library close any
 * more: the worker task's own AmiSSL init/instance and bsdsocket base stay
 * open across every station switch and are only closed once, by
 * radio_net_worker_entry()'s own teardown when Radio_NetworkShutdown() asks
 * the worker to exit. Radio_Close() used to call radio_net_close_child()
 * here; there is nothing left for it to do per station. */

static void radio_ssl_close_stream_mode(RadioStream *rs, RadioCloseMode mode);
static void radio_ssl_close_stream(RadioStream *rs);
static void radio_ssl_free_ctx(RadioStream *rs);
static void radio_abort_current_socket(RadioStream *rs);
static void close_current_socket(RadioStream *rs);

/* Every non-blocking SSL_connect/SSL_read/SSL_write call site classifies its
 * SSL_get_error() result into three buckets: WANT_READ/WANT_WRITE (retry
 * later, nothing wrong), ZERO_RETURN (a clean TLS close_notify EOF -- the
 * existing reconnect/end-of-stream path handles this safely, no quarantine
 * needed), and everything else, which is fatal. That third bucket used to be
 * narrowed to "SSL_ERROR_SSL or a non-empty OpenSSL error queue", which
 * missed SSL_ERROR_SYSCALL with an empty queue (a bare socket-level failure
 * AmiSSL couldn't attach a record-layer reason to) and any other/unknown
 * SSL_get_error() code -- letting SSL_free()/SSL_CTX_free() run on an SSL
 * object that just took a fatal I/O fault, corrupting AmiSSL's internals
 * (observed as an AN_BadFreeAddr 0100000F recoverable alert). Treat every
 * outcome outside the two known-safe buckets as fatal instead. */
static int radio_ssl_error_is_fatal(int e)
{
    return e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE &&
        e != SSL_ERROR_ZERO_RETURN;
}

/* Poll SSL_connect on the non-blocking socket — same budget as radio_wait_connected. */
static int radio_ssl_do_handshake(RadioStream *rs)
{
    int tries;
    int last_error = 0;
    radio_net_adopt_context(rs);
    /* Start with a clean OpenSSL error queue: a stale entry left by an
     * earlier failed connection would otherwise be misread as this
     * connection's fatal error by the fault handling below. */
    ERR_clear_error();
    for (tries = 0; tries < 150; tries++) {
        int r, e;
        if (radio_is_stopping(rs)) return -1;
        RADIO_DBG(printf("BEFORE SSL_connect session=%lu attempt=%d ssl=%p ctx=%p fd=%ld\n",
            rs ? rs->session_id : 0, tries + 1, rs ? (void *)rs->ssl : 0,
            rs ? (void *)rs->ctx : 0, rs ? (long)rs->sock : -1L););
        r = SSL_connect(rs->ssl);
        if (r == 1) {
            RADIO_DBG(printf("AFTER SSL_connect success session=%lu attempt=%d ssl=%p ctx=%p fd=%ld\n",
                rs ? rs->session_id : 0, tries + 1, rs ? (void *)rs->ssl : 0,
                rs ? (void *)rs->ctx : 0, rs ? (long)rs->sock : -1L););
            RADIO_DBG(printf("SSL_CONNECT_ATTEMPT session=%lu attempt=%d ret=%d err=0 fd=%ld ssl=%p ctx=%p handshake=%d\n", rs ? rs->session_id : 0, tries + 1, r, rs ? (long)rs->sock : -1L, rs ? (void *)rs->ssl : 0, rs ? (void *)rs->ctx : 0, rs ? rs->sslHandshakeDone : 0););
            if (rs) rs->sslHandshakeDone = 1;
            RADIO_DBG(printf("SSL_CONNECT_DONE session=%lu\n", rs ? rs->session_id : 0););
            return 0;
        }
        e = SSL_get_error(rs->ssl, r);
        RADIO_DBG(printf("AFTER SSL_connect fail session=%lu attempt=%d ret=%d err=%d ssl=%p ctx=%p fd=%ld\n",
            rs ? rs->session_id : 0, tries + 1, r, e, rs ? (void *)rs->ssl : 0,
            rs ? (void *)rs->ctx : 0, rs ? (long)rs->sock : -1L););
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
            if (rs && radio_ssl_error_is_fatal(e)) {
                rs->lastSslError = e;
                rs->noReconnect = 1;
                strcpy(rs->lastSslOp, (e == SSL_ERROR_SYSCALL && ssl_lib_error == 0) ?
                    "ssl-connect-syscall" : "ssl-connect-fatal");
                RADIO_DBG(printf("radio-tls: session=%lu handshake failed; normal cleanup will free SSL/CTX and allow another HTTPS station\n", rs->session_id));
            }
            /* Drain the rest of the queue so the failure cannot masquerade
             * as a later session's fault. */
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
    radio_net_adopt_context(rs);
    if (radio_net_worker_amissl_ready(rs) != 0) return -1;
    if (rs->ctx && !rs->ctxFreed) radio_ssl_free_ctx(rs);
    if (!rs->ctx) {
        method = SSLv23_client_method();
        if (!method) { set_error(rs, "AmiSSL init failed"); return -1; }
        RADIO_DBG(printf("BEFORE SSL_CTX_new session=%lu method=%p\n",
            rs->session_id, (void *)method););
        rs->ctx = SSL_CTX_new(method);
        RADIO_DBG(printf("AFTER SSL_CTX_new session=%lu ctx=%p\n",
            rs->session_id, (void *)rs->ctx););
        if (rs->ctx) { rs->ctxFreed = 0; radio_active_ssl_ctx_count++; RADIO_DBG(printf("radio-resource: session=%lu SSL_CTX allocated active_ssl_ctx_count=%ld\n", rs->session_id, radio_active_ssl_ctx_count)); }
        if (!rs->ctx) { set_error(rs, "AmiSSL init failed"); return -1; }
        SSL_CTX_set_verify(rs->ctx, SSL_VERIFY_NONE, NULL);
#ifdef SSL_OP_IGNORE_UNEXPECTED_EOF
        /* OpenSSL 3.x (AmiSSL v5): treat a peer that drops the connection
         * without close_notify as SSL_ERROR_ZERO_RETURN. Streaming servers
         * drop connections abruptly, and each HTTPS attempt now has fresh
         * SSL/CTX objects that are cleaned up after the attempt. */
        SSL_CTX_set_options(rs->ctx, SSL_OP_IGNORE_UNEXPECTED_EOF);
#endif
    }
    RADIO_DBG(printf("BEFORE SSL_new session=%lu ctx=%p\n",
        rs->session_id, (void *)rs->ctx););
    rs->ssl = SSL_new(rs->ctx);
    RADIO_DBG(printf("AFTER SSL_new session=%lu ssl=%p ctx=%p\n",
        rs->session_id, (void *)rs->ssl, (void *)rs->ctx););
    if (rs->ssl) {
        rs->sslFreed = 0;
        rs->sslReadCloseSeen = 0;
        radio_active_ssl_count++;
        RADIO_DBG(printf("radio-resource: session=%lu SSL allocated active_ssl_count=%ld\n", rs->session_id, radio_active_ssl_count));
    }
    if (!rs->ssl) { radio_ssl_close_stream(rs); set_error(rs, "AmiSSL init failed"); return -1; }
#ifdef SSL_CTRL_SET_TLSEXT_HOSTNAME
    SSL_set_tlsext_host_name(rs->ssl, rs->host);
#endif
#ifdef RADIO_SSL_VERIFY_PEER
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
    radio_net_adopt_context(rs);
    RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: HTTPS cleanup start mode=%s ssl=%p ctx=%p fd=%ld handshake=%d\n", radio_close_mode_name(mode), (void *)rs->ssl, (void *)rs->ctx, (long)rs->sock, rs->sslHandshakeDone));
    if (mode == RADIO_CLOSE_GRACEFUL && !Radio_IsMemoryPoisoned() && rs->ssl && rs->sslHandshakeDone && rs->sock != RADIO_INVALID_SOCKET && !rs->socketClosed) {
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
    if (rs->ssl && !rs->sslFreed) {
        RADIO_DBG(printf("radio-safety: pre-SSL_free guard session=%lu ssl=%p ctx=%p fd=%ld lastSslError=%d lastSslOp=%s memoryPoisoned=%d\n",
            rs->session_id, (void *)rs->ssl, (void *)rs->ctx, (long)rs->sock,
            rs->lastSslError, rs->lastSslOp[0] ? rs->lastSslOp : "none",
            Radio_IsMemoryPoisoned()));
        if (Radio_IsMemoryPoisoned()) {
            RADIO_DBG(printf("radio-cleanup: SSL_free skipped (memory poison) session=%lu ssl=%p leaking to avoid heap damage\n",
                rs->session_id, (void *)rs->ssl));
            radio_amissl_task_poisoned = 1;
            radio_tls_shutdown_quarantine = 1;
        } else {
            RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: SSL_free start ssl=%p\n", (void *)rs->ssl));
            RADIO_DBG(printf("BEFORE SSL_free session=%lu ssl=%p ctx=%p\n",
                rs->session_id, (void *)rs->ssl, (void *)rs->ctx););
            SSL_free(rs->ssl);
            rs->ssl_free_count++;
            RADIO_DBG(printf("AFTER SSL_free session=%lu ssl_free_count=%u\n",
                rs->session_id, rs->ssl_free_count););
            RADIO_DBG(printf("radio-cleanup: SSL_free done session=%lu\n", rs->session_id));
        }
        rs->sslFreed = 1;
        if (radio_active_ssl_count > 0) radio_active_ssl_count--;
        rs->ssl = NULL;
        rs->sslHandshakeDone = 0;
    } else {
        RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: SSL_free skipped\n"));
    }
    radio_ssl_free_ctx(rs);
    RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: HTTPS cleanup complete\n"));
}

/* Safe default used by every error/timeout/handshake-failure path: never
 * attempts SSL_shutdown() on a session that may already be in an error or
 * peer-closed state. */
static void radio_ssl_close_stream(RadioStream *rs) { radio_ssl_close_stream_mode(rs, RADIO_CLOSE_ABORT); }

/* Free the per-attempt SSL_CTX. HTTPS attempts do not reuse SSL_CTX objects. */
static void radio_ssl_free_ctx_local(RadioStream *rs)
{
    if (!rs) return;
    radio_net_adopt_context(rs);
    if (rs->ctx && !rs->ctxFreed) {
        if (Radio_IsMemoryPoisoned()) {
            RADIO_DBG(printf("radio-cleanup: SSL_CTX_free skipped (memory poison) session=%lu ctx=%p leaking to avoid heap damage\n",
                rs->session_id, (void *)rs->ctx));
            radio_amissl_task_poisoned = 1;
            radio_tls_shutdown_quarantine = 1;
        } else {
            RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: SSL_CTX_free start ctx=%p\n", (void *)rs->ctx));
            RADIO_DBG(printf("BEFORE SSL_CTX_free session=%lu ctx=%p\n",
                rs->session_id, (void *)rs->ctx););
            SSL_CTX_free(rs->ctx);
            rs->ssl_ctx_free_count++;
            RADIO_DBG(printf("AFTER SSL_CTX_free session=%lu ssl_ctx_free_count=%u\n",
                rs->session_id, rs->ssl_ctx_free_count););
            RADIO_DBG(printf("radio-cleanup: SSL_CTX_free done session=%lu\n", rs->session_id));
        }
        rs->ctxFreed = 1;
        if (radio_active_ssl_ctx_count > 0) radio_active_ssl_ctx_count--;
        rs->ctx = NULL;
    } else {
        RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: SSL_CTX_free skipped\n"));
    }
}

static void radio_ssl_free_ctx_job(void *arg) { radio_ssl_free_ctx_local((RadioStream *)arg); }

/* Self-dispatching: SSL_CTX_free() touches AmiSSL, so it may only run on the
 * net worker task. Called both from worker-context helpers further down in
 * this file (radio_ssl_connect(), radio_ssl_close_stream_mode() -- where
 * radio_net_worker_is_self() is already true, so this just falls through to
 * the local call with no IPC overhead) and from foreign-task entry points
 * (Radio_Close()), which need the dispatch. */
static void radio_ssl_free_ctx(RadioStream *rs)
{
    if (!rs) return;
    if (radio_net_worker_is_self()) { radio_ssl_free_ctx_local(rs); return; }
    Radio_RunOnNetWorker(radio_ssl_free_ctx_job, rs);
}
#endif /* AMIGA_M68K && HAVE_AMISSL */

#if !defined(AMIGA_M68K) || !defined(HAVE_AMISSL)
static void radio_net_adopt_context(RadioStream *rs) { (void)rs; }
int Radio_PlaybackOwnsNetwork(void) { return 0; }
#endif

/* Drive a non-blocking connect() to completion by re-issuing connect() and
 * yielding with Delay() between tries, so the connect never blocks (and so
 * never freezes WinUAE's emulation).  Returns 0 on success, -1 on failure or
 * stop/timeout.  Success is connect()==0 or EISCONN; anything else is treated
 * as "still connecting" until the timeout, which keeps us robust even if a
 * particular stack reports a non-standard in-progress errno. */
static int radio_wait_connected(RadioStream *rs, struct sockaddr_in *sa)
{
    int tries;
    radio_net_adopt_context(rs);
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
    radio_net_adopt_context(rs);
    while (sent < len && tries < 150) {
        int r;
        if (radio_is_stopping(rs))
            return -1;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
        if (rs->isSSL && rs->ssl) {
            radio_net_adopt_context(rs);
            RADIO_DBG(printf("radio-ssl-write: session=%lu sslHandshakeDone=%d before SSL_write\n", rs->session_id, rs->sslHandshakeDone););
            if (rs->sslHandshakeDone != 1) {
                RADIO_DBG(printf("radio-ssl-write: ERROR session=%lu skipped SSL_write because handshake is incomplete sslHandshakeDone=%d\n", rs->session_id, rs->sslHandshakeDone););
                set_error(rs, "TLS handshake incomplete");
                close_current_socket(rs);
                return -1;
            }
            r = (int)SSL_write(rs->ssl, buf + sent, len - sent);
            if (r > 0) { sent += r; continue; }
            {
                int e = SSL_get_error(rs->ssl, r);
                unsigned long ssl_lib_error;
                if (e == SSL_ERROR_WANT_WRITE || e == SSL_ERROR_WANT_READ) {
                    radio_backoff_sleep(); tries++; continue;
                }
                /* Normal TLS write failure: record it and let the per-attempt cleanup free SSL/CTX. */
                ssl_lib_error = ERR_get_error();
                RADIO_DBG(printf("radio-ssl-write: session=%lu write failed ssl_error=%d lib_error=%08lx\n", rs->session_id, e, ssl_lib_error));
                if (radio_ssl_error_is_fatal(e)) {
                    rs->lastSslError = e;
                    rs->noReconnect = 1;
                    strcpy(rs->lastSslOp, (e == SSL_ERROR_SYSCALL && ssl_lib_error == 0) ?
                        "ssl-write-syscall" : "ssl-write-fatal");
                    RADIO_DBG(printf("radio-ssl-write: session=%lu fatal write failure; normal cleanup will free SSL/CTX\n", rs->session_id));
                    ERR_clear_error();
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

static void set_status(RadioStream *rs, RadioStatus status) { if (rs) { radio_stream_lock(rs); if (rs->status != RADIO_STATUS_ERROR && rs->status != RADIO_STATUS_CLOSED && rs->status != RADIO_STATUS_STOPPING) rs->status = status; radio_stream_unlock(rs); } }

static void radio_copy_metadata_string(char *dst, size_t dstSize, const char *src, const char *label)
{
    size_t rawLen, safeLen;
    if (!dst || dstSize == 0)
        return;
    if (!src)
        src = "";
    rawLen = strlen(src);
    safeLen = AmigaUtf8ToDisplay(dst, dstSize, src);
    RADIO_DBG(printf("radio-icy: sanitized %s rawLen=%lu sanitizedLen=%lu dstSize=%lu%s\n",
        label ? label : "metadata", (unsigned long)rawLen, (unsigned long)safeLen,
        (unsigned long)dstSize, (rawLen && safeLen + 1 >= dstSize) ? " truncated" : ""););
}

static void radio_copy_metadata_bytes(char *dst, size_t dstSize, const unsigned char *src, int srcLen, const char *label)
{
    size_t rawLen, safeLen;
    char raw[RADIO_META_MAX];
    if (!dst || dstSize == 0)
        return;
    rawLen = (src && srcLen > 0) ? (size_t)srcLen : 0;
    if (rawLen >= sizeof(raw))
        rawLen = sizeof(raw) - 1;
    if (src && rawLen > 0)
        memcpy(raw, src, rawLen);
    raw[rawLen] = 0;
    safeLen = AmigaUtf8ToDisplay(dst, dstSize, raw);
    RADIO_DBG(printf("radio-icy: sanitized %s rawLen=%lu sanitizedLen=%lu dstSize=%lu%s\n",
        label ? label : "metadata", (unsigned long)((src && srcLen > 0) ? (size_t)srcLen : 0), (unsigned long)safeLen,
        (unsigned long)dstSize, (src && srcLen > 0 && safeLen + 1 >= dstSize) ? " truncated" : ""););
}

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

static void set_error(RadioStream *rs, const char *msg) { if (rs) { radio_stream_lock(rs); radio_copy_string(rs->error,sizeof(rs->error),msg); rs->status = RADIO_STATUS_ERROR; radio_stream_unlock(rs); RADIO_OPEN_DEBUG_PRINTF(("radio-open: %s\n", msg ? msg : "error")); } }
static void radio_ring_set_canary(RadioStream *rs);
static int radio_ring_check_canary(RadioStream *rs, const char *where);
static int ring_write(RadioStream *rs, const unsigned char *p, int n)
{
    int written = 0;
    if (!rs || !p || n <= 0 || !rs->ring || !rs->size) return 0;
    if (radio_ring_check_canary(rs, "before ring_write") < 0) return 0;
    radio_stream_lock(rs);
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
    }
    if (written == 0) {
        rs->ringLastWrite = 0;
        RADIO_DBG(printf("radio-ring: no write session=%lu icy_state=%d metaint=%d requested=%d fill=%lu ring_free=%lu wpos=%lu capacity=%lu url=\"%s\"\n",
            rs->session_id, (int)rs->parseState, rs->metaint, n, rs->used,
            rs->size > rs->used ? rs->size - rs->used : 0, rs->wpos, rs->size, rs->url););
    }
    radio_stream_unlock(rs);
    radio_ring_check_canary(rs, "after ring_write");
    return written;
}
static int ring_read(RadioStream *rs, unsigned char *p, int n) { int i=0; if(!rs||!p||n<=0)return 0; radio_stream_lock(rs); if(!rs->headerDone||!rs->decoderStarted){ RADIO_DBG(printf("radio-guard: ring_read refused session=%lu headerDone=%d decoderStarted=%d firstData=%d status=%d used=%lu\n", rs->session_id, rs->headerDone, rs->decoderStarted, rs->firstDataLogged, (int)rs->status, rs->used);); radio_stream_unlock(rs); return 0; } radio_stream_unlock(rs); if (radio_ring_check_canary(rs, "before ring_read") < 0) return 0; radio_stream_lock(rs); while (i<n && rs->used) { p[i++]=rs->ring[rs->rpos++]; if(rs->rpos>=rs->size)rs->rpos=0; rs->used--; } radio_stream_unlock(rs); radio_ring_check_canary(rs, "after ring_read"); return i; }
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
        rs->fatalStop = 1;
        rs->noReconnect = 1;
        rs->lastSslError = 0;
        strcpy(rs->lastSslOp, "ring-corruption");
        radio_amissl_task_poisoned = 1;
        strcpy(radio_amissl_task_poison_reason, "ring-corruption");
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
    if (*slash && strlen(slash) >= sizeof(rs->path)) {
        /* Refuse rather than truncate: a cut-off path (usually the end of
         * an auth token) makes the server respond in ways that have proven
         * fatal to AmiSSL's record parser. */
        RADIO_DBG(printf("radio-open: path too long (%lu >= %lu) url=\"%s\"\n",
            (unsigned long)strlen(slash), (unsigned long)sizeof(rs->path), url));
        return -1;
    }
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
    struct sockaddr_in sa; char req[1024]; int n; int cr;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    /* connect_http() only ever runs on the net worker task now (dispatched
     * via Radio_RunOnNetWorker() from Radio_OpenWithHostAddr()), so
     * SocketBase is already the worker's own, opened once at worker
     * start-up -- nothing left to open/adopt per station here. isSSL
     * streams additionally need the worker's AmiSSL instance; that is
     * checked inside radio_ssl_connect() below via
     * radio_net_worker_amissl_ready(). */
    RADIO_DBG(printf("radio-net-worker: session=%lu worker context active before DNS/socket/connect/TLS SocketBase=%p AmiSSLMasterBase=%p AmiSSLBase=%p AmiSSLExtBase=%p initialized=%d\n",
        rs->session_id, (void *)SocketBase, (void *)AmiSSLMasterBase, (void *)AmiSSLBase,
        (void *)AmiSSLExtBase, radio_amissl_initialized););
#elif defined(AMIGA_M68K)
    radio_net_adopt_context(rs);
#endif
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
        RADIO_DBG(printf("radio-child-net: before DNS session=%lu host=%s SocketBase=%p\n",
            rs->session_id, rs->host, (void *)SocketBase););
        RADIO_DBG(printf("radio-dns: WARNING blocking DNS lookup in playback child host=%s\n", rs->host););
#if defined(AMIGA_M68K)
        /* gethostbyname() is a genuinely blocking bsdsocket.library call with
         * no non-blocking mode of its own, unlike connect()/recv() elsewhere
         * in this file.  Without a break mask, the Stop button's
         * SIGBREAKF_CTRL_C (see StopPlayback() in both front ends) cannot
         * touch it: the task just sits inside the resolver until it answers
         * or times out, which some nameservers/stacks never do quickly --
         * exactly the "Stopping..." that never finishes when a stream is
         * asked to stop while it's still resolving its host (e.g. the user
         * switches stations again before the previous one finished
         * connecting).  SBTC_BREAKMASK tells bsdsocket.library which signals
         * should abort a blocking call early for the calling task.
         *
         * Scoped tightly to just this one call: it is cleared again
         * immediately below, win or lose.  Leaving it set would also apply
         * to every later blocking bsdsocket call this task makes, including
         * whatever AmiSSL does internally inside SSL_connect()/SSL_read() a
         * few lines below for HTTPS streams -- and AmiSSL's SSL_connect() is
         * already documented above (radio_net_worker_entry()) as sensitive to
         * exactly this kind of external signal interference, having
         * previously gone unresponsive to Stop when the surrounding signal
         * plumbing changed. Getting an unexpected EINTR-style abort deep
         * inside a library that was never written to expect one is a
         * plausible way to corrupt its state rather than cleanly cancel it,
         * so the mask must not outlive this single call. */
        SocketBaseTags(SBTM_SETVAL(SBTC_BREAKMASK), (ULONG)SIGBREAKF_CTRL_C, TAG_DONE);
        he=gethostbyname(rs->host);
        SocketBaseTags(SBTM_SETVAL(SBTC_BREAKMASK), 0UL, TAG_DONE);
#else
        he=gethostbyname(rs->host);
#endif
        if(!he || !he->h_addr){
            if (radio_is_stopping(rs)) { set_error(rs,"stopped"); return -1; }
            set_error(rs,"cannot resolve stream host"); RADIO_OPEN_DEBUG_PRINTF(("radio-open: DNS failed for %s\n", rs->host)); return -1;
        }
        memcpy(&rs->hostAddr, he->h_addr, sizeof(rs->hostAddr));
        rs->haveHostAddr=1;
    }
    if (radio_is_stopping(rs)) return -1;
    RADIO_DBG(printf("radio-child-net: before socket session=%lu host=%s SocketBase=%p\n",
        rs->session_id, rs->host, (void *)SocketBase););
    rs->sock=socket(AF_INET,SOCK_STREAM,0); if(rs->sock!=RADIO_INVALID_SOCKET){ rs->socketClosed = 0; rs->closeCleanupDone = 0; radio_open_socket_count++; radio_playback_open_socket_count++; RADIO_DBG(printf("radio-socket: playback socket opened session=%lu host=%s fd=%ld open_socket_count=%ld playback_open_socket_count=%ld\n", rs->session_id, rs->host, (long)rs->sock, radio_open_socket_count, radio_playback_open_socket_count);) } if(rs->sock==RADIO_INVALID_SOCKET){ radio_log_socket_failure(rs, "playback", "socket"); set_error(rs,"Cannot create socket - TCP stack may still be releasing previous stream"); return -1; }
    /* Go non-blocking BEFORE connect so the connect never stalls the machine. */
    radio_set_nonblocking(rs->sock);
    memset(&sa,0,sizeof(sa)); sa.sin_family=AF_INET; sa.sin_port=htons((unsigned short)rs->port); sa.sin_addr=rs->hostAddr;
    if (radio_is_stopping(rs)) { close_current_socket(rs); return -1; }
    RADIO_DBG(printf("radio-child-net: before connect session=%lu fd=%ld host=%s SocketBase=%p\n",
        rs->session_id, (long)rs->sock, rs->host, (void *)SocketBase););
    RADIO_DBG(printf("radio-connect: session=%lu initial connect() fd=%ld host=%s port=%d\n", rs->session_id, (long)rs->sock, rs->host, rs->port););
    cr=connect(rs->sock,(struct sockaddr*)&sa,sizeof(sa));
    RADIO_DBG(printf("radio-connect: session=%lu initial connect() cr=%d errno=%ld\n", rs->session_id, cr, cr < 0 ? radio_sock_errno() : 0L););
    if(cr<0 && radio_wait_connected(rs,&sa)!=0){ close_current_socket(rs); set_error(rs,"TCP connect failed"); RADIO_OPEN_DEBUG_PRINTF(("radio-open: connect failed to %s:%d\n", rs->host, rs->port)); return -1; }
    rs->streamStateFlags |= TRANSPORT_CONNECTED;
    if (radio_is_stopping(rs)) { close_current_socket(rs); return -1; }
    RADIO_DBG(printf("radio-connect: session=%lu TCP connected, starting SSL/HTTP request phase isSSL=%d\n", rs->session_id, rs->isSSL););
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    if (rs->isSSL) {
        RADIO_DBG(printf("radio-child-net: before TLS session=%lu fd=%ld host=%s SocketBase=%p AmiSSLBase=%p AmiSSLExtBase=%p\n",
            rs->session_id, (long)rs->sock, rs->host, (void *)SocketBase,
            (void *)AmiSSLBase, (void *)AmiSSLExtBase););
        rs->sslHandshakeDone = 0;
        if (radio_ssl_connect(rs) != 0) { close_current_socket(rs); return -1; }
        rs->streamStateFlags |= TLS_DONE;
        if (radio_is_stopping(rs)) { close_current_socket(rs); return -1; }
    }
#endif
    RADIO_DBG(printf("radio-http: session=%lu starting HTTP request phase isSSL=%d sslHandshakeDone=%d\n", rs->session_id, rs->isSSL, rs->sslHandshakeDone););
    n=snprintf(req,sizeof(req),"GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: BoingPlayer/0.1 AmigaOS\r\nIcy-MetaData: 1\r\nConnection: close\r\n\r\n",rs->path,rs->host);
    if(n<0 || n>=(int)sizeof(req)){
        /* Never send a truncated request: a cut-off auth token is exactly
         * what provoked the malformed server response behind the
         * "invalid record" AmiSSL fault chain. */
        close_current_socket(rs); set_error(rs,"stream URL too long"); return -1;
    }
    if(radio_send_all(rs,req,n)!=0){ close_current_socket(rs); set_error(rs, rs->isSSL ? "HTTPS read failed" : "cannot send HTTP request"); return -1; }
    reset_parser(rs);
    rs->decoderStarted = rs->playbackStarted = 0;
    rs->streamStateFlags &= (STREAM_ALLOCATED | TRANSPORT_CONNECTED | TLS_DONE);
    return 0;
}

#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
typedef struct RadioOpenJobArgs {
    RadioStream *rs;
    int result;
} RadioOpenJobArgs;

/* connect_http()/close_current_socket() touch bsdsocket.library/AmiSSL, so
 * they may only run on the net worker task -- Radio_OpenWithHostAddr() runs
 * on whichever task is starting this station (the playback child) and hands
 * this closure to Radio_RunOnNetWorker() instead of calling them directly. */
static void radio_worker_job_open(void *arg)
{
    RadioOpenJobArgs *a = (RadioOpenJobArgs *)arg;
    a->result = connect_http(a->rs);
    if (a->result == 0) radio_worker_register_stream(a->rs);
    else close_current_socket(a->rs);
}
#endif

static void radio_abort_current_socket_local(RadioStream *rs)
{
    if (!rs) return;
    radio_net_adopt_context(rs);
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
        RADIO_DBG(printf("BEFORE CloseSocket session=%lu fd=%ld open_socket_count=%ld playback_open_socket_count=%ld\n",
            rs->session_id, (long)rs->sock, radio_open_socket_count,
            radio_playback_open_socket_count););
        rs->socket_close_count++;
        radio_close_socket(rs->sock);
        RADIO_DBG(printf("AFTER CloseSocket session=%lu fd=%ld socket_close_count=%u\n",
            rs->session_id, closing_fd, rs->socket_close_count););
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

#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
static void radio_abort_current_socket_job(void *arg) { radio_abort_current_socket_local((RadioStream *)arg); }
#endif

/* Self-dispatching: CloseSocket() (and, via rs->ssl, the SSL_get_rbio()/
 * BIO_set_fd() detach above) touches bsdsocket.library/AmiSSL, so it may
 * only run on the net worker task. Radio_RequestStop() calls this directly
 * from whichever (foreign) task owns this session; close_current_socket_
 * mode() below (itself self-dispatching) calls it again once already
 * running on the worker task, where radio_net_worker_is_self() short-
 * circuits straight to the local call with no IPC round trip. */
static void radio_abort_current_socket(RadioStream *rs)
{
    if (!rs) return;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    if (!radio_net_worker_is_self()) { Radio_RunOnNetWorker(radio_abort_current_socket_job, rs); return; }
#endif
    radio_abort_current_socket_local(rs);
}

static void close_current_socket_mode_local(RadioStream *rs, RadioCloseMode mode)
{
    long before;
    if (!rs) return;
    radio_net_adopt_context(rs);
    radio_stream_magic_valid(rs, "close_current_socket");
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    if (rs->sock == RADIO_INVALID_SOCKET && rs->socketClosed &&
        (!rs->ssl || rs->sslFreed)) {
#else
    if (rs->sock == RADIO_INVALID_SOCKET && rs->socketClosed) {
#endif
        /* Already fully closed: nothing left to abort or free. A session
         * marked fatalStop/noReconnect (or one bouncing through repeated
         * abort calls before this was added) could otherwise re-enter this
         * whole sequence -- and re-log it -- on every pump. Log once, not
         * hundreds of times. */
        if (!rs->closeCleanupDone) {
            rs->closeCleanupDone = 1;
            RADIO_DBG(printf("radio-cleanup: close mode=%s session=%lu already closed -- no-op\n",
                radio_close_mode_name(mode), rs->session_id));
        }
        return;
    }
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

#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
typedef struct RadioCloseModeJobArgs {
    RadioStream *rs;
    RadioCloseMode mode;
} RadioCloseModeJobArgs;
static void close_current_socket_mode_job(void *arg)
{
    RadioCloseModeJobArgs *a = (RadioCloseModeJobArgs *)arg;
    close_current_socket_mode_local(a->rs, a->mode);
}
#endif

/* Self-dispatching, same reasoning as radio_abort_current_socket() above:
 * this (and everything it calls -- radio_ssl_close_stream_mode(),
 * radio_abort_current_socket()) touches bsdsocket.library/AmiSSL and may
 * only run on the net worker task. Called directly from foreign-task entry
 * points (Radio_Close(), Radio_ReadStartupAudio(), Radio_FailStartup()) as
 * well as from worker-context helpers (connect_http(), radio_pump_body(),
 * reconnect_http()) that are already running on the worker task by the time
 * they get here. */
static void close_current_socket_mode(RadioStream *rs, RadioCloseMode mode)
{
    if (!rs) return;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    if (!radio_net_worker_is_self()) {
        RadioCloseModeJobArgs args;
        args.rs = rs;
        args.mode = mode;
        Radio_RunOnNetWorker(close_current_socket_mode_job, &args);
        return;
    }
#endif
    close_current_socket_mode_local(rs, mode);
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
    if (rs && rs->noReconnect) {
        /* A fatal SSL fault already marked this session terminal -- never
         * re-run connect_http()'s DNS/socket/SSL_connect against a task
         * whose AmiSSL state that fault may have damaged. */
        set_error(rs, "TLS read failed");
        return -1;
    }
    if (radio_is_stopping(rs)) { if (rs) rs->status = RADIO_STATUS_CLOSED; return -1; }
    if (rs->reconnectAttempts >= RADIO_RECONNECT_MAX) { set_error(rs,"radio reconnect attempts exhausted"); return -1; }
    if (rs->reconnectDelay > 0) { rs->reconnectDelay--; set_status(rs, RADIO_STATUS_RECONNECTING); radio_backoff_sleep(); return 0; }
    rs->reconnectAttempts++;
    set_status(rs, rs->reconnectAttempts == 1 ? RADIO_STATUS_CONNECTING : RADIO_STATUS_RECONNECTING);
    if (connect_http(rs) == 0) { set_status(rs, RADIO_STATUS_BUFFERING); return 1; }
    if (rs->noReconnect) {
        /* connect_http() just hit a fatal SSL_connect fault (see
         * radio_ssl_do_handshake()): do not schedule yet another reconnect
         * attempt for this call either, fail now. */
        set_error(rs, "TLS read failed");
        return -1;
    }
    rs->reconnectDelay = RADIO_RECONNECT_BACKOFF_PUMPS * rs->reconnectAttempts;
    set_status(rs, RADIO_STATUS_RECONNECTING);
    return 0;
}

static void parse_headers(RadioStream *rs,char *h){ if (radio_stream_magic_valid(rs, "parse_headers") < 1) return; char *line=strtok(h,"\r\n"); int code=0; if(line && ci_starts(line,"ICY")) code=200; else if(line && ci_starts(line,"HTTP/")) sscanf(line,"HTTP/%*s %d",&code); else { set_error(rs,"invalid HTTP stream response"); RADIO_OPEN_DEBUG_PRINTF(("radio-open: HTTP header failed\n")); return; } if(code<200||code>299){ char msg[64]; sprintf(msg,"HTTP %d stream error",code); set_error(rs,msg); RADIO_OPEN_DEBUG_PRINTF(("radio-open: HTTP header failed status %d\n", code)); return; } while((line=strtok(NULL,"\r\n"))){ char *v=strchr(line,':'); if(!v) continue; *v++=0; line=trim(line); v=trim(v); if(ci_equals(line,"Content-Type")) radio_copy_string(rs->contentType,sizeof(rs->contentType),v); else if(ci_equals(line,"icy-metaint")){ rs->metaint=atoi(v); rs->audioUntilMeta=rs->metaint; } else if(ci_equals(line,"icy-br")) rs->bitrate=atoi(v); else if(ci_equals(line,"icy-name")) radio_copy_metadata_string(rs->stationName,sizeof(rs->stationName),v,"icy-name"); else if(ci_equals(line,"icy-genre")) radio_copy_metadata_string(rs->genre,sizeof(rs->genre),v,"icy-genre"); else if(ci_equals(line,"icy-url")) radio_copy_string(rs->streamUrl,sizeof(rs->streamUrl),v); } if(rs->contentType[0] && (ci_starts(rs->contentType,"application/vnd.apple.mpegurl") || ci_starts(rs->contentType,"application/x-mpegurl"))) { set_error(rs,"HLS stream not supported"); RADIO_OPEN_DEBUG_PRINTF(("radio-open: HLS content type unsupported: %s\n", rs->contentType)); return; } RADIO_OPEN_DEBUG_PRINTF(("radio-open: final URL=%s content-type=%s URL codec hint=%s final selected codec=%s\n",
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
            radio_copy_metadata_bytes(rs->title, sizeof(rs->title), p, i, "StreamTitle");
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
            if (!rs->firstMetaLogged) {
                rs->firstMetaLogged = 1;
                RADIO_DBG(printf("radio-icy: first metadata block session=%lu len_byte=%d meta_len=%d metaint=%d fill=%lu url=\"%s\"\n",
                    rs->session_id, lenByte, rs->metaLen, rs->metaint, rs->used, rs->url););
            }
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
            if (wrote > 0 && rs->headerDone && !rs->decoderStarted) { rs->decoderStarted = 1; rs->streamStateFlags |= DECODER_STARTED; radio_active_decoder_count++; RADIO_DBG(printf("radio-resource: session=%lu decoder path started active_decoder_count=%ld first_write=%d fill=%lu metaint=%d\n", rs->session_id, radio_active_decoder_count, wrote, rs->used, rs->metaint);); }
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
    if (rs->status == RADIO_STATUS_ERROR) {
        /* Already failed for a specific reason (e.g. a fatal TLS fault
         * already set "TLS read failed" via set_error() this same
         * Radio_Pump() call) -- a generic startup-timeout message here would
         * silently overwrite and hide the real diagnosis. First error wins. */
        return -1;
    }
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
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    /* Recorded before anything else can run a worker job for this session --
     * see the struct field comment (radio_is_stopping()'s cross-task
     * SIGBREAKF_CTRL_C bridge). */
    rs->requestingTask = FindTask(NULL);
#endif
    if (!radio_atexit_registered) { atexit(radio_app_exit_report); radio_atexit_registered = 1; }
    radio_reset_session_state(rs);
    rs->session_id = radio_next_session_id++;
    if (radioMemoryPoisoned) {
        rs->status = RADIO_STATUS_ERROR;
        radio_copy_string(rs->url, sizeof(rs->url), url ? url : "");
        set_error(rs, "Memory corruption detected; restart MiniAMP3 before playing radio.");
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
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    {
        RadioOpenJobArgs openArgs;
        openArgs.rs = rs;
        openArgs.result = -1;
        if (!Radio_RunOnNetWorker(radio_worker_job_open, &openArgs))
            set_error(rs, "AmiSSL unavailable: network worker not running");
        else if (openArgs.result == 0) {
            rs->status = RADIO_STATUS_BUFFERING;
            radio_debug_mem_report(rs->session_id, "after HTTP request phase started");
        } else if (rs->status != RADIO_STATUS_ERROR)
            set_error(rs, rs->error[0] ? rs->error : "cannot open radio stream");
    }
    if (rs->status == RADIO_STATUS_ERROR) {
        radio_debug_mem_report(rs->session_id, "after failed probe cleanup");
        radio_resource_summary(rs, "after failed probe cleanup");
        RADIO_OPEN_DEBUG_PRINTF(("radio-open: Radio_Open returning error\n"));
    }
#else
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
#endif
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
    radio_stream_lock(rs);
    rs->stopping = 1;
    rs->reconnectAttempts = RADIO_RECONNECT_MAX;
    rs->reconnectDelay = 0;
    rs->status = RADIO_STATUS_STOPPING;
    radio_stream_unlock(rs);
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
    RADIO_DBG(printf("radio-teardown: before Radio_Close second stop phase (Radio_RequestStop re-entry) session=%lu\n", rs->session_id));
    Radio_RequestStop(rs);
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    if (!radio_net_worker_is_self()) {
        if (!Radio_RunOnNetWorker(radio_worker_unregister_stream_job, rs)) {
            radio_stream_lock(rs);
            rs->workerDetached = 1;
            radio_stream_unlock(rs);
            RADIO_DBG(printf("radio-net-worker: unregister timed out for session=%lu -- leaking RadioStream/ring because worker may still touch them\n", rs->session_id););
            return;
        }
    } else
        radio_worker_unregister_stream(rs);
    {
        int workerClosedAck;
        radio_stream_lock(rs);
        workerClosedAck = rs->workerClosedAck;
        if (!workerClosedAck) rs->workerDetached = 1;
        radio_stream_unlock(rs);
        if (!workerClosedAck) {
            RADIO_DBG(printf("radio-net-worker: unregister returned without close ack for session=%lu -- leaking RadioStream/ring because worker ownership is unclear\n", rs->session_id););
            return;
        }
    }
#endif
    close_current_socket(rs);
    rs->status = RADIO_STATUS_CLOSED;
    rs->stream_buffer_free_count++;
    rs->audio_buffer_free_count++;
    {
        /* Fatal TLS teardown quarantine: once a fatal SSL fault or ring
         * corruption has poisoned this session (Radio_IsSessionFatal()),
         * the whole object graph is unsafe -- not just the SSL/SSL_CTX
         * objects already gated below. Skip the ring buffer free too and
         * leak it, same reasoning as the corrupted-canary leak this already
         * did for a narrower case. */
        int fatal = Radio_IsSessionFatal(rs);
        Radio_DebugCheckExecMem("before ring buffer free/skip");
        if (rs->stream_buffer_free_count > 1) radio_duplicate_cleanup_warning(rs, "stream buffer free", rs->stream_buffer_free_count);
        else { if (rs->ringAlloc) {
            /* radio_ring_check_canary() returning -1 means the guard bytes
             * around this block are already corrupted -- in a HEAPGUARD debug
             * build, MiniMem_Free() defensively refuses to free a block whose
             * header doesn't look valid, but a release build has no such
             * safety net: calling the real free() on an already-corrupted
             * block risks smashing the allocator's own free-list instead of
             * just leaking one buffer. Leak it deliberately instead. */
            if (!fatal && radio_ring_check_canary(rs, "before cleanup") == 0)
                free(rs->ringAlloc);
            else
                RADIO_DBG(printf("radio-cleanup: session=%lu url=\"%s\" ring buffer free skipped (%s), quarantining/leaking to avoid smashing the allocator\n",
                    rs->session_id, rs->url, fatal ? "fatal TLS quarantine" : "corrupted"));
            if (radio_active_stream_buffer_count > 0) radio_active_stream_buffer_count--;
        } }
        Radio_DebugCheckExecMem("after ring buffer free/skip");
    }
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
    /* bsdsocket.library, amisslmaster.library and the AmiSSL instance itself
     * are all owned by the single long-lived net worker task and stay open
     * across every station switch -- see radio_net_worker_entry(). Only this
     * session's own per-connection SSL_CTX (if any survived close_current_
     * socket()'s call chain) needs freeing here; there is no per-task AmiSSL
     * context to close per station any more. radio_ssl_free_ctx() is
     * self-dispatching (it runs on the worker task even though Radio_Close()
     * itself runs on the playback child), same as close_current_socket(). */
    Radio_DebugCheckExecMem("before SSL_CTX_free/skip");
    radio_ssl_free_ctx(rs);
    Radio_DebugCheckExecMem("after SSL_CTX_free/skip");
    rs->amissl_cleanup_count++;
#endif
    radio_debug_mem_report(rs->session_id, "after stop cleanup");
    radio_resource_summary(rs, "after stop cleanup");
    RADIO_STOP_DEBUG_PRINTF(("radio-stop: stream task exiting / Radio_Close exited session=%lu task_exit_count=%u cleanup_count=%u stop_request_count=%u ssl_free_count=%u socket_close_count=%u decoder_free_count=%u\n", rs->session_id, rs->task_exit_count, rs->cleanup_count, rs->stop_request_count, rs->ssl_free_count, rs->socket_close_count, rs->decoder_free_count));
    if (Radio_IsSessionFatal(rs)) {
        /* The RadioStream struct itself is part of the unsafe object graph
         * once a fatal TLS fault or ring corruption has been detected: leak
         * it rather than free() it. This mirrors the ring-buffer/SSL/
         * SSL_CTX/CleanupAmiSSL quarantine above -- a leak is better than
         * risking a free-list write into already-damaged exec memory. */
        RADIO_DBG(printf("radio-cleanup: RadioStream struct free skipped (fatal TLS quarantine) session=%lu -- leaking\n", rs->session_id));
    } else {
        free(rs);
    }
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

/* Full set of per-session resource counters, for a caller that wants to
 * confirm every prior radio session's resources have actually been released
 * before starting a new one (a queued station switch, say) -- not just that
 * the old child Task has been reaped by DOS. */
void Radio_GetTeardownStats(long *active_stream_sessions, long *active_stream_tasks,
    long *open_socket_count, long *playback_open_socket_count,
    long *active_decoder_count, long *active_audio_buffer_count,
    long *active_stream_buffer_count)
{
    if (active_stream_sessions) *active_stream_sessions = radio_active_stream_sessions;
    if (active_stream_tasks) *active_stream_tasks = radio_active_stream_tasks;
    if (open_socket_count) *open_socket_count = radio_open_socket_count;
    if (playback_open_socket_count) *playback_open_socket_count = radio_playback_open_socket_count;
    if (active_decoder_count) *active_decoder_count = radio_active_decoder_count;
    if (active_audio_buffer_count) *active_audio_buffer_count = radio_active_audio_buffer_count;
    if (active_stream_buffer_count) *active_stream_buffer_count = radio_active_stream_buffer_count;
}

/* The GUI/opener task (and every other subsystem: radio_browser_http.c,
 * etc.) always gets NULL here in HAVE_AMISSL builds -- the net worker task's
 * SocketBase/AmiSSLBase/AmiSSLMasterBase are private to that one task and
 * are never read, swapped, or shared with any other task.  radio_stream_
 * probe.c's own code only ever runs via Radio_RunOnNetWorker(), i.e. it IS
 * the worker task by the time it calls this, so it (and only it) still gets
 * the real pointers -- see Radio_GetAmiSslShared()'s comment below. */
void Radio_GetNetworkBases(void **socket_base, void **amissl_base, void **amissl_master_base)
{
#if defined(AMIGA_M68K)
#if defined(HAVE_AMISSL)
    if (radio_net_worker_is_self()) {
        if (socket_base) *socket_base = (void *)SocketBase;
        if (amissl_base) *amissl_base = (void *)AmiSSLBase;
        if (amissl_master_base) *amissl_master_base = (void *)AmiSSLMasterBase;
        return;
    }
#else
    /* Plain-HTTP-only m68k build: no worker task, bsdsocket.library is a
     * single ordinary shared library opened once by Radio_NetworkInit() --
     * unchanged from before this file's single-worker rework. */
    if (socket_base) *socket_base = (void *)SocketBase;
    if (amissl_base) *amissl_base = 0;
    if (amissl_master_base) *amissl_master_base = 0;
    return;
#endif
#endif
    if (socket_base) *socket_base = 0;
    if (amissl_base) *amissl_base = 0;
    if (amissl_master_base) *amissl_master_base = 0;
}

/* True once the net worker task (HAVE_AMISSL builds) or Radio_NetworkInit()
 * itself (plain-HTTP builds) has successfully opened bsdsocket.library --
 * lets the GUI grey out internet-radio features up front on a machine with
 * no network stack installed, instead of failing later on first connect.
 * A plain status flag, not the SocketBase pointer itself. */
int Radio_HasNetwork(void)
{
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    return radio_net_worker_libs_ok != 0;
#elif defined(AMIGA_M68K)
    return SocketBase != NULL;
#else
    return 0;
#endif
}

/* True once the net worker task has successfully opened its AmiSSL instance
 * -- lets the GUI grey out the HTTPS scheme option up front on a machine
 * without AmiSSL installed (always false in builds without HAVE_AMISSL). */
int Radio_HasHttps(void)
{
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    return radio_net_worker_https_ok != 0;
#else
    return 0;
#endif
}

/* Runtime accessor so radio_stream_probe.c can use the net worker's AmiSSL
 * instance instead of opening a second one of its own.  The probe file
 * declares its own AmiSSLBase/AmiSSLExtBase/AmiSSLMasterBase as weak symbols
 * (required by the AmiSSL proto-header call stubs, which resolve through a
 * global of that exact name in whichever translation unit calls them) that
 * do not reliably merge with this file's strong definitions under the m68k
 * hunk linker, so it adopts the values through this function call instead --
 * which works with every linker AND (unlike the old design) never exposes
 * the worker's bases to any task other than the worker itself: probe.c's
 * code now only ever runs via Radio_RunOnNetWorker(), so by the time it
 * calls this, it IS the worker task. */
void Radio_GetAmiSslShared(void **amissl_base, void **amissl_ext_base, void **amissl_master_base)
{
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    if (radio_net_worker_is_self()) {
        if (amissl_base) *amissl_base = (void *)AmiSSLBase;
        if (amissl_ext_base) *amissl_ext_base = (void *)AmiSSLExtBase;
        if (amissl_master_base) *amissl_master_base = (void *)AmiSSLMasterBase;
        return;
    }
#endif
    if (amissl_base) *amissl_base = 0;
    if (amissl_ext_base) *amissl_ext_base = 0;
    if (amissl_master_base) *amissl_master_base = 0;
}

/* Start the single long-lived radio net worker task (HAVE_AMISSL builds) or
 * open bsdsocket.library directly (plain-HTTP-only m68k builds without
 * AmiSSL, which have no worker task and no per-task AmiSSL lifecycle to
 * manage). Safe to call more than once -- a no-op if already up -- and safe
 * even if the caller never uses HTTPS/radio at all. */
void Radio_NetworkInit(void)
{
#if defined(AMIGA_M68K)
#if defined(HAVE_AMISSL)
    if (!radio_net_worker_ensure_started())
        RADIO_DBG(printf("radio-netinit: net worker start/AmiSSL open failed (HTTPS unavailable this run)\n"););
#else
    if (!SocketBase) {
        SocketBase = OpenLibrary("bsdsocket.library", 4);
        if (SocketBase) {
            radio_socket_library_open_count++;
            RADIO_DBG(printf("radio-netinit: bsdsocket.library opened socket_library_open_count=%ld\n", radio_socket_library_open_count););
        }
    }
#endif
    Radio_DebugCheckExecMem("after Radio_NetworkInit");
#endif
}

/* Final network-library teardown at application exit, called only after
 * every playback child has been stopped and reaped.  This is now the only
 * place bsdsocket.library/AmiSSL are closed -- the net worker task itself is
 * asked to close them and exit exactly once, here. */
void Radio_NetworkShutdown(void)
{
#if defined(AMIGA_M68K)
    if (radio_network_shutdown_started) {
        RADIO_DBG(printf("radio-netshutdown: already ran, skipping duplicate shutdown\n"););
        return;
    }
    radio_network_shutdown_started = 1;
#if defined(HAVE_AMISSL)
    if (Radio_IsTlsPoisoned() || radio_tls_shutdown_quarantine) {
        /* A fatal SSL fault or detected memory corruption already left the
         * worker's per-task AmiSSL state (or the heap itself) suspect this
         * run -- asking the same worker to run CleanupAmiSSL()-adjacent
         * teardown (CloseAmiSSL()/CloseLibrary()) against damaged internals
         * is exactly the class of "recoverable alert on app close" this
         * quarantine exists to avoid. Abandon the worker task and its
         * libraries instead: the OS reclaims the open library counts when
         * the process exits, same as before. */
        if (Radio_IsTlsPoisoned())
            printf("APP_CLOSE: AmiSSL poisoned, skipping final AmiSSL shutdown\n");
        else
            printf("APP_CLOSE: AmiSSL quarantined after TLS fault(s), skipping final AmiSSL shutdown\n");
        RADIO_DBG(printf("radio-netshutdown: AmiSSL %s (reason=%s, tls_fault_count=%ld), abandoning net worker task=%p without asking it to CloseAmiSSL/CloseLibrary\n",
            Radio_IsTlsPoisoned() ? "poisoned" : "quarantined",
            Radio_TlsPoisonReason(), radio_tls_fault_count, (void *)radio_net_worker_task););
    } else {
        /* Ask the worker task to CloseAmiSSL()/CloseLibrary() (in that
         * order, exactly once, matching amissl_child_worker_repro.c) and
         * exit; radio_net_worker_stop() waits, bounded, for it to finish. */
        if (!radio_net_worker_stop()) {
            RADIO_DBG(printf("radio-netshutdown: net worker did not confirm shutdown within the timeout\n"););
        } else {
            radio_closeamissl_count++;
            radio_amisslmaster_close_count++;
            radio_socket_library_close_count++;
        }
    }
#else
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
#endif
    RADIO_DBG(printf("radio-netshutdown: final counters socket_library_open_count=%ld socket_library_close_count=%ld amisslmaster_open_count=%ld amisslmaster_close_count=%ld openamissltags_count=%ld closeamissl_count=%ld amissl_init_count=%ld amissl_cleanup_count=%ld active_socket_count=%ld active_ssl_count=%ld active_ssl_ctx_count=%ld tls_poisoned=%d poison_reason=\"%s\"\n",
        radio_socket_library_open_count, radio_socket_library_close_count,
        radio_amisslmaster_open_count, radio_amisslmaster_close_count,
        radio_openamissltags_count, radio_closeamissl_count,
        radio_amissl_init_count, radio_amissl_cleanup_count,
        radio_open_socket_count, radio_active_ssl_count,
        radio_active_ssl_ctx_count, Radio_IsTlsPoisoned(),
        Radio_TlsPoisonReason()););
    RADIO_DBG(printf("radio-netshutdown: tls_fault_count=%ld shutdown_quarantine=%d\n",
        radio_tls_fault_count, radio_tls_shutdown_quarantine););
#endif
}

/* The bulk of Radio_Pump()'s work (SSL_read()/recv(), reconnect_http(),
 * close_current_socket() on error/stop) touches bsdsocket.library/AmiSSL, so
 * in HAVE_AMISSL builds this only ever runs on the net worker task
 * -- the net worker's autonomous loop calls this for registered streams.
 * Radio_Pump() itself is intentionally only a cheap status/buffer check in
 * those builds. Non-AmiSSL builds still call it directly, unchanged. */
static int radio_pump_body(RadioStream *rs)
{
    unsigned char b[1024];
    int n, wb;
    if (!rs || rs->status == RADIO_STATUS_ERROR) return -1;
    radio_net_adopt_context(rs);
    if (rs->fatalStop) { set_error(rs, "TLS read failed"); return -1; }
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
        radio_net_adopt_context(rs);
        n = (int)SSL_read(rs->ssl, (char *)b, requested);
        RADIO_DBG(printf("radio-ssl-read: session=%lu ssl=%p ctx=%p fd=%ld dst=%p dst_cap=%d requested=%d returned=%d fill=%lu ring_free=%lu\n",
            rs->session_id, (void *)rs->ssl, (void *)rs->ctx, (long)rs->sock, (void *)b, (int)sizeof(b), requested, n, rs->used, rs->size > rs->used ? rs->size - rs->used : 0));
        if (n <= 0) {
            int e = SSL_get_error(rs->ssl, n);
            RADIO_DBG(printf("radio-ssl-read: session=%lu SSL_get_error=%d ret=%d fd=%ld\n", rs->session_id, e, n, (long)rs->sock));
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) wb = 1;
            else if (e == SSL_ERROR_ZERO_RETURN) {
                RADIO_DBG(printf("radio-ssl-read: session=%lu SSL_ERROR_ZERO_RETURN clean close\n", rs->session_id));
            }
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
                if (radio_ssl_error_is_fatal(e)) {
                    rs->lastSslError = e;
                    rs->noReconnect = 1;
                    strcpy(rs->lastSslOp, (e == SSL_ERROR_SYSCALL && ssl_lib_error == 0) ?
                        "ssl-read-syscall" : "ssl-read-fatal");
                    RADIO_DBG(printf("radio-ssl-read: session=%lu fatal read failure; normal cleanup will free SSL/CTX\n", rs->session_id));
                    ERR_clear_error();
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
        if (rs->everPlayed && ++rs->stallPumps >= RADIO_STALL_TIMEOUT_PUMPS) {
            /* Fail the stream through the same set_error/close path as the
             * startup timeout (radio_note_start_wait) so the decoder unwinds
             * and the playback child exits cleanly.  Deliberately NOT an
             * automatic reconnect: tearing down and re-creating the AmiSSL
             * session unattended exercises the SSL_free()/SSL_CTX churn this
             * file's fault-quarantine comments exist for, and doing that in a
             * loop against an already-dead relay risks exactly the alerts the
             * quarantine defends against.  The user can restart the stream
             * from a clean session with one click instead. */
            RADIO_DBG(printf("radio-stream: session=%lu stalled for %d silent pumps, failing stream url=\"%s\"\n", rs->session_id, rs->stallPumps, rs->url););
            rs->stallPumps = 0;
            set_error(rs, "radio stream stalled - no data received");
            close_current_socket(rs);
            return -1;
        }
        return 0;
    }
    if (n <= 0) {
        close_current_socket(rs);
        if (rs->fatalStop || rs->noReconnect) {
            /* A fatal SSL fault (SSL_ERROR_SSL/SYSCALL/unknown) was just
             * classified above: this session is terminal, full stop. Every
             * other branch below either waits for the AAC/HTTPS stream-start
             * timeout or schedules a reconnect -- both re-enter
             * SSL_connect()/SSL_read() against per-task AmiSSL state this
             * fault may have already damaged, which is what looped
             * "close mode=abort" for a long time and eventually corrupted
             * the exec heap. Fail the stream now instead. */
            set_error(rs, "TLS read failed");
            RADIO_DBG(printf("radio-stream: session=%lu TLS/socket failure marked noReconnect -- refusing reconnect/timeout wait, failing stream\n", rs->session_id));
            return -1;
        }
        if (!rs->headerDone || !rs->firstDataLogged) { rs->decoderStarted = 0; rs->streamStateFlags &= ~DECODER_STARTED; set_error(rs, rs->isSSL ? "HTTPS read failed" : "HTTP header read failed"); RADIO_OPEN_DEBUG_PRINTF(("radio-open: HTTP header failed before ready headerDone=%d firstData=%d\n", rs->headerDone, rs->firstDataLogged)); return -1; }
        if (!rs->everPlayed) { set_error(rs, "radio stream ended before audio buffered"); return -1; }
        rs->reconnectDelay = RADIO_RECONNECT_BACKOFF_PUMPS;
        set_status(rs, RADIO_STATUS_RECONNECTING);
        return 0;
    }
    rs->zeroBytePumps = 0;
    rs->stallPumps = 0;
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
            Radio_DebugCheckExecMem("playing 10s sample");
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

int Radio_Pump(RadioStream *rs)
{
    if (!rs) return -1;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    {
        RadioStatus status;
        unsigned long used;
        int headerDone;
        radio_stream_lock(rs);
        status = rs->status;
        used = rs->used;
        headerDone = rs->headerDone;
        radio_stream_unlock(rs);
        if (status == RADIO_STATUS_ERROR) return -1;
        if (status == RADIO_STATUS_CLOSED || status == RADIO_STATUS_STOPPING) return 0;
        if (headerDone && used >= RADIO_START_THRESHOLD && status != RADIO_STATUS_PLAYING)
            set_status(rs, RADIO_STATUS_PLAYING);
        if (used > 0) return (int)used;
        if (status == RADIO_STATUS_CONNECTING || status == RADIO_STATUS_BUFFERING || status == RADIO_STATUS_RECONNECTING)
            radio_backoff_sleep();
        return 0;
    }
#else
    return radio_pump_body(rs);
#endif
}

int Radio_ReadAudio(RadioStream *rs,unsigned char *buf,int maxBytes){ int got; if(!rs||!buf||maxBytes<=0)return 0; if(radio_is_stopping(rs)) return 0; while(!radio_is_stopping(rs) && rs->status!=RADIO_STATUS_PLAYING && rs->used<RADIO_START_THRESHOLD && rs->status!=RADIO_STATUS_ERROR) { if(Radio_Pump(rs)<=0 && !rs->everPlayed && (++rs->zeroBytePumps>=RADIO_ZERO_BYTE_PUMP_MAX || radio_note_start_wait(rs,"radio stream did not buffer audio")<0)) { if(rs->status!=RADIO_STATUS_ERROR) set_error(rs,"radio stream did not buffer audio"); break; } } while(!radio_is_stopping(rs) && rs->used==0 && rs->status!=RADIO_STATUS_ERROR) { if(Radio_Pump(rs)<=0 && !rs->everPlayed && (++rs->zeroBytePumps>=RADIO_ZERO_BYTE_PUMP_MAX || radio_note_start_wait(rs,"radio stream did not deliver audio")<0)) { if(rs->status!=RADIO_STATUS_ERROR) set_error(rs,"radio stream did not deliver audio"); break; } } if(radio_is_stopping(rs)||!rs->headerDone||!rs->decoderStarted||rs->status==RADIO_STATUS_ERROR) return 0; got=ring_read(rs,buf,maxBytes); if(!rs->everPlayed && rs->status==RADIO_STATUS_PLAYING && rs->used<RADIO_LOW_WATER_BYTES) set_status(rs,RADIO_STATUS_BUFFERING); if(rs->status==RADIO_STATUS_BUFFERING && rs->used>=RADIO_START_THRESHOLD) set_status(rs,RADIO_STATUS_PLAYING); return got; }
int Radio_ReadStartupAudio(RadioStream *rs,unsigned char *buf,int maxBytes,unsigned long timeoutMs){ clock_t start; int got; if(!rs||!buf||maxBytes<=0)return 0; start=clock(); while(!radio_is_stopping(rs)&&rs->used==0&&rs->status!=RADIO_STATUS_ERROR){ if(Radio_Pump(rs)<0)break; if(timeoutMs>0 && (unsigned long)((clock()-start)*1000UL/CLOCKS_PER_SEC)>=timeoutMs){ set_error(rs,"AAC stream start timeout"); close_current_socket(rs); break; } } if(radio_is_stopping(rs)||!rs->headerDone||!rs->decoderStarted||rs->status==RADIO_STATUS_ERROR) return 0; got=ring_read(rs,buf,maxBytes); if(!rs->everPlayed&&rs->headerDone&&rs->status!=RADIO_STATUS_PLAYING&&rs->status!=RADIO_STATUS_ERROR) set_status(rs,RADIO_STATUS_BUFFERING); return got; }
void Radio_FailStartup(RadioStream *rs,const char *message){ if(!rs)return; set_error(rs,message&&message[0]?message:"AAC stream start timeout"); radio_stream_lock(rs); rs->stopping=1; rs->reconnectAttempts=RADIO_RECONNECT_MAX; rs->reconnectDelay=0; radio_stream_unlock(rs); close_current_socket(rs); }
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
unsigned long Radio_GetSessionId(RadioStream *rs){ return rs?rs->session_id:0; }
int Radio_IsSessionFatal(RadioStream *rs){
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    return rs ? (rs->sslStatePoisoned || rs->fatalStop) : 0;
#else
    return rs ? rs->fatalStop : 0;
#endif
}
const char *Radio_StatusText(RadioStatus s){ switch(s){case RADIO_STATUS_CONNECTING:return "Connecting";case RADIO_STATUS_BUFFERING:return "Buffering";case RADIO_STATUS_PLAYING:return "Playing";case RADIO_STATUS_RECONNECTING:return "Reconnecting";case RADIO_STATUS_STOPPING:return "Stopping";case RADIO_STATUS_CLOSED:return "Closed";case RADIO_STATUS_ERROR:return "Error";default:return "Idle";} }

#endif /* ENABLE_RADIO */
