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
#ifdef RADIO_DEBUG_OPEN
#define RADIO_OPEN_DEBUG_PRINTF(x) printf x
#else
#define RADIO_OPEN_DEBUG_PRINTF(x) ((void)0)
#endif
#undef RADIO_STOP_DEBUG_PRINTF
#if RADIO_DEBUG_STOP
#define RADIO_STOP_DEBUG_PRINTF(x) printf x
#define RADIO_CLEANUP_DEBUG_PRINTF(x) printf x
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

struct RadioStream {
    RADIO_SOCKET sock;
    RadioStatus status;
    char url[256], host[128], path[192];
    int port, bitrate, metaint, audioUntilMeta, headerDone;
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
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    SSL *ssl;
    SSL_CTX *ctx;
    int sslHandshakeDone;
#endif
};

static unsigned long radio_next_session_id = 1;
static long radio_active_stream_sessions = 0;
static long radio_active_stream_tasks = 0;
static long radio_open_socket_count = 0;
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

static void radio_debug_mem_report(unsigned long session_id, const char *where)
{
#if defined(AMIGA_M68K)
    printf("radio-mem: session=%lu %s AvailMem(any)=%lu fast=%lu chip=%lu\n",
        session_id, where ? where : "", (unsigned long)AvailMem(MEMF_ANY),
        (unsigned long)AvailMem(MEMF_FAST), (unsigned long)AvailMem(MEMF_CHIP));
#else
    printf("radio-mem: session=%lu %s AvailMem(any)=n/a fast=n/a chip=n/a\n",
        session_id, where ? where : "");
#endif
}

static void radio_resource_summary(const RadioStream *rs, const char *where)
{
    printf("radio-summary: session=%lu %s active_stream_sessions=%ld active_stream_tasks=%ld open_socket_count=%ld active_ssl_count=%ld active_ssl_ctx_count=%ld active_decoder_count=%ld active_audio_buffer_count=%ld active_stream_buffer_count=%ld active_icy_metadata_count=%ld active_gui_nodes=%ld active_gui_strings=%ld amissl_init_count=%ld cleanup_count=%u\n",
        rs ? rs->session_id : 0, where ? where : "", radio_active_stream_sessions,
        radio_active_stream_tasks, radio_open_socket_count, radio_active_ssl_count,
        radio_active_ssl_ctx_count, radio_active_decoder_count,
        radio_active_audio_buffer_count, radio_active_stream_buffer_count,
        radio_active_icy_metadata_count, radio_gui_listbrowser_node_count,
        radio_gui_string_count, radio_amissl_init_count, rs ? rs->cleanup_count : 0);
}
static void radio_app_exit_report(void)
{
    radio_debug_mem_report(0, "before app close");
    radio_resource_summary(NULL, "before app close");
}

static int radio_is_stopping(const RadioStream *rs) { return !rs || rs->stopping || rs->status == RADIO_STATUS_STOPPING || rs->status == RADIO_STATUS_CLOSED; }
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
        printf("radio-cleanup warning: session=%lu duplicate %s count=%u; skipping duplicate operation\n", rs->session_id, what, count);
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
    rs->contentType[0] = rs->title[0] = rs->stationName[0] = rs->genre[0] = rs->streamUrl[0] = rs->error[0] = 0;
    rs->rpos = rs->wpos = rs->used = 0;
    rs->headerLen = 0; rs->header[0] = 0;
    rs->parseState = RADIO_PARSE_HEADER;
    rs->metaLen = rs->metaGot = rs->metaLeft = 0;
    rs->reconnectAttempts = rs->reconnectDelay = rs->zeroBytePumps = rs->startPumps = 0;
    rs->everPlayed = rs->firstDataLogged = rs->haveHostAddr = 0;
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

/* Forward declaration so the SSL helpers can call set_error before it is
 * defined later in this translation unit. */
static void set_error(RadioStream *rs, const char *msg);

#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
static int radio_ssl_global_init(RadioStream *rs)
{
    if (!SocketBase) {
        SocketBase = OpenLibrary("bsdsocket.library", 4);
        if (!SocketBase) { set_error(rs, "AmiSSL unavailable: bsdsocket.library unavailable"); return -1; }
    }
    if (AmiSSLBase && radio_amissl_initialized) return 0;
    if (!AmiSSLMasterBase) {
        AmiSSLMasterBase = OpenLibrary("amisslmaster.library", AMISSLMASTER_MIN_VERSION);
        if (!AmiSSLMasterBase) { set_error(rs, "AmiSSL unavailable: amisslmaster.library unavailable"); return -1; }
        if (!InitAmiSSLMaster(AMISSL_CURRENT_VERSION, TRUE)) {
            CloseLibrary(AmiSSLMasterBase); AmiSSLMasterBase = NULL;
            set_error(rs, "AmiSSL init failed"); return -1;
        }
    }
    if (OpenAmiSSLTags(AMISSL_CURRENT_VERSION,
                       AmiSSL_UsesOpenSSLStructs, TRUE,
                       AmiSSL_GetAmiSSLBase, (ULONG)&AmiSSLBase,
                       AmiSSL_GetAmiSSLExtBase, (ULONG)&AmiSSLExtBase,
                       AmiSSL_SocketBase, (ULONG)SocketBase,
                       AmiSSL_ErrNoPtr, (ULONG)&errno,
                       TAG_DONE) != 0) {
        set_error(rs, "AmiSSL unavailable"); return -1;
    }
    if (InitAmiSSL(AmiSSL_SocketBase, (ULONG)SocketBase,
                   AmiSSL_ErrNoPtr, (ULONG)&errno,
                   TAG_DONE) != 0) {
        CloseAmiSSL(); AmiSSLBase = NULL; AmiSSLExtBase = NULL;
        set_error(rs, "AmiSSL init failed"); return -1;
    }
    radio_amissl_initialized = 1;
    radio_amissl_init_count++;
    printf("radio-resource: session=%lu AmiSSL init count=%ld\n", rs ? rs->session_id : 0, radio_amissl_init_count);
    return 0;
}

static void radio_ssl_global_cleanup(void)
{
    printf("radio-ssl-diag: cleanup ENTER initialized=%d base=%p ext=%p master=%p\n", radio_amissl_initialized, (void *)AmiSSLBase, (void *)AmiSSLExtBase, (void *)AmiSSLMasterBase);
    RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: AmiSSL global cleanup start initialized=%d base=%p master=%p\n", radio_amissl_initialized, (void *)AmiSSLBase, (void *)AmiSSLMasterBase));
    if (radio_amissl_initialized) {
        RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: CleanupAmiSSL start\n"));
        CleanupAmiSSL(TAG_DONE);
        if (radio_amissl_init_count > 0) radio_amissl_init_count--;
        radio_amissl_initialized = 0;
        RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: CleanupAmiSSL done\n"));
    } else {
        RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: CleanupAmiSSL skipped\n"));
    }
    if (AmiSSLBase) {
        RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: CloseAmiSSL start base=%p\n", (void *)AmiSSLBase));
        CloseAmiSSL();
        AmiSSLBase = NULL;
        AmiSSLExtBase = NULL;
        RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: CloseAmiSSL done\n"));
    } else {
        RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: CloseAmiSSL skipped\n"));
    }
    /* Keep amisslmaster.library open for the lifetime of the program.  AmiSSL
     * requires InitAmiSSLMaster() to run exactly once; only the per-task
     * OpenAmiSSL()/CloseAmiSSL() pair above is allowed to repeat.  The previous
     * code closed and re-opened the master library on every stream stop, so a
     * stop->probe->start cycle (Play pressed on an already-playing stream) re-ran
     * InitAmiSSLMaster() several times in quick succession and wedged the next
     * HTTPS connection, freezing the machine.  The master base is shared with
     * radio_stream_probe.c through the weak AmiSSLMasterBase symbol and the OS
     * reclaims it at program exit. */
    RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: amisslmaster kept open master=%p\n", (void *)AmiSSLMasterBase));
    RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: AmiSSL global cleanup complete\n"));
    printf("radio-ssl-diag: cleanup EXIT  initialized=%d base=%p ext=%p master=%p\n", radio_amissl_initialized, (void *)AmiSSLBase, (void *)AmiSSLExtBase, (void *)AmiSSLMasterBase);
}

/* Poll SSL_connect on the non-blocking socket — same budget as radio_wait_connected. */
static int radio_ssl_do_handshake(RadioStream *rs)
{
    int tries;
    for (tries = 0; tries < 150; tries++) {
        int r, e;
        if (radio_is_stopping(rs)) return -1;
        r = SSL_connect(rs->ssl);
        if (r == 1) return 0;
        e = SSL_get_error(rs->ssl, r);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
            radio_backoff_sleep(); continue;
        }
        return -1;
    }
    return -1;
}

static int radio_ssl_connect(RadioStream *rs)
{
    const SSL_METHOD *method;
    if (radio_ssl_global_init(rs) != 0) return -1;
    method = SSLv23_client_method();
    if (!method) { set_error(rs, "AmiSSL init failed"); return -1; }
    rs->ctx = SSL_CTX_new(method);
    if (rs->ctx) { radio_active_ssl_ctx_count++; printf("radio-resource: session=%lu SSL_CTX allocated active_ssl_ctx_count=%ld\n", rs->session_id, radio_active_ssl_ctx_count); }
    if (!rs->ctx) { set_error(rs, "AmiSSL init failed"); return -1; }
    SSL_CTX_set_verify(rs->ctx, SSL_VERIFY_NONE, NULL);
    rs->ssl = SSL_new(rs->ctx);
    if (rs->ssl) { radio_active_ssl_count++; printf("radio-resource: session=%lu SSL allocated active_ssl_count=%ld\n", rs->session_id, radio_active_ssl_count); }
    if (!rs->ssl) { SSL_CTX_free(rs->ctx); rs->ctx = NULL; if (radio_active_ssl_ctx_count > 0) radio_active_ssl_ctx_count--; set_error(rs, "AmiSSL init failed"); return -1; }
    SSL_set_fd(rs->ssl, (int)rs->sock);
    if (radio_ssl_do_handshake(rs) != 0) {
        SSL_free(rs->ssl); rs->ssl = NULL; if (radio_active_ssl_count > 0) radio_active_ssl_count--;
        if (rs->ctx) { SSL_CTX_free(rs->ctx); rs->ctx = NULL; if (radio_active_ssl_ctx_count > 0) radio_active_ssl_ctx_count--; }
        rs->sslHandshakeDone = 0;
        set_error(rs, "TLS handshake failed"); return -1;
    }
    rs->sslHandshakeDone = 1;
    return 0;
}

static void radio_ssl_close_stream(RadioStream *rs)
{
    if (!rs) return;
    RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: HTTPS cleanup start ssl=%p ctx=%p fd=%ld handshake=%d\n", (void *)rs->ssl, (void *)rs->ctx, (long)rs->sock, rs->sslHandshakeDone));
    if (rs->ssl && rs->sslHandshakeDone && rs->sock != RADIO_INVALID_SOCKET) {
        RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: SSL_shutdown start ssl=%p\n", (void *)rs->ssl));
        SSL_shutdown(rs->ssl);
        RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: SSL_shutdown done\n"));
    } else {
        RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: SSL_shutdown skipped ssl=%p fd=%ld handshake=%d\n", (void *)rs->ssl, (long)rs->sock, rs->sslHandshakeDone));
    }
    if (rs->ssl) {
        RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: SSL_free start ssl=%p\n", (void *)rs->ssl));
        rs->ssl_free_count++;
        SSL_free(rs->ssl);
        if (radio_active_ssl_count > 0) radio_active_ssl_count--;
        rs->ssl = NULL;
        rs->sslHandshakeDone = 0;
        RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: SSL_free done\n"));
    } else {
        RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: SSL_free skipped\n"));
    }
    if (rs->ctx) {
        RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: SSL_CTX_free start ctx=%p\n", (void *)rs->ctx));
        rs->ssl_ctx_free_count++;
        SSL_CTX_free(rs->ctx);
        if (radio_active_ssl_ctx_count > 0) radio_active_ssl_ctx_count--;
        rs->ctx = NULL;
        RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: SSL_CTX_free done\n"));
    } else {
        RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: SSL_CTX_free skipped\n"));
    }
    RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: HTTPS cleanup complete\n"));
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
    /* ~6s budget at 40ms/poll; generous for a slow stream server. */
    for (tries = 0; tries < 150; tries++) {
        long e;
        if (radio_is_stopping(rs))
            return -1;
        radio_backoff_sleep();
        if (radio_is_stopping(rs))
            return -1;
        if (connect(rs->sock, (struct sockaddr *)sa, sizeof(*sa)) == 0)
            return 0;
        e = radio_sock_errno();
        if (e == EISCONN)
            return 0;
    }
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
    if (copyLen >= dstSize)
        copyLen = dstSize - 1;
    memcpy(dst, src, copyLen);
    dst[copyLen] = 0;
}

static void set_error(RadioStream *rs, const char *msg) { if (rs) { radio_copy_string(rs->error,sizeof(rs->error),msg); rs->status = RADIO_STATUS_ERROR; RADIO_OPEN_DEBUG_PRINTF(("radio-open: %s\n", msg ? msg : "error")); } }
static void radio_ring_set_canary(RadioStream *rs);
static int radio_ring_check_canary(RadioStream *rs, const char *where);
static int ring_write(RadioStream *rs, const unsigned char *p, int n) { int i=0; if (radio_ring_check_canary(rs, "before ring_write") < 0) return 0; while (i<n && rs->used<rs->size) { rs->ring[rs->wpos++]=p[i++]; if(rs->wpos>=rs->size)rs->wpos=0; rs->used++; } rs->ringLastWrite=(unsigned long)i; radio_ring_check_canary(rs, "after ring_write"); return i; }
static int ring_read(RadioStream *rs, unsigned char *p, int n) { int i=0; if (radio_ring_check_canary(rs, "before ring_read") < 0) return 0; while (i<n && rs->used) { p[i++]=rs->ring[rs->rpos++]; if(rs->rpos>=rs->size)rs->rpos=0; rs->used--; } radio_ring_check_canary(rs, "after ring_read"); return i; }
static int ci_starts(const char *s,const char *p){ while(*p) { if(tolower((unsigned char)*s++)!=tolower((unsigned char)*p++)) return 0; } return 1; }
static int ci_equals(const char *a,const char *b){ while(*a&&*b){ if(tolower((unsigned char)*a++)!=tolower((unsigned char)*b++)) return 0; } return *a==0&&*b==0; }
static char *trim(char *s){ char *e; while(*s&&isspace((unsigned char)*s))s++; e=s+strlen(s); while(e>s&&isspace((unsigned char)e[-1]))*--e=0; return s; }
static void close_current_socket(RadioStream *rs);

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
        printf("radio-canary: CORRUPTED buffer=stream/audio ring session=%lu where=%s expected_capacity=%lu last_write_size=%lu codec=%s url=\"%s\"\n",
            rs->session_id, where ? where : "", rs->size, rs->ringLastWrite,
            ((ci_starts(rs->contentType,"audio/aac") || ci_starts(rs->contentType,"audio/aacp") || radio_contains_nocase(rs->path,"aac")) ? "AAC" : "MP3/unknown"),
            rs->url);
        rs->stopping = 1;
        rs->status = RADIO_STATUS_STOPPING;
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
        printf("radio-dns: session=%lu using cached probe DNS %s\n", rs->session_id, addr_text);
    } else {
        struct hostent *he;
        printf("radio-dns: WARNING blocking DNS lookup in playback child host=%s\n", rs->host);
        he=gethostbyname(rs->host);
        if(!he || !he->h_addr){ set_error(rs,"cannot resolve stream host"); RADIO_OPEN_DEBUG_PRINTF(("radio-open: DNS failed for %s\n", rs->host)); return -1; }
        memcpy(&rs->hostAddr, he->h_addr, sizeof(rs->hostAddr));
        rs->haveHostAddr=1;
    }
    if (radio_is_stopping(rs)) return -1;
    rs->sock=socket(AF_INET,SOCK_STREAM,0); if(rs->sock!=RADIO_INVALID_SOCKET){ radio_open_socket_count++; printf("radio-resource: session=%lu socket opened fd=%ld open_socket_count=%ld\n", rs->session_id, (long)rs->sock, radio_open_socket_count); } if(rs->sock==RADIO_INVALID_SOCKET){ set_error(rs,"cannot create socket"); return -1; }
    /* Go non-blocking BEFORE connect so the connect never stalls the machine. */
    radio_set_nonblocking(rs->sock);
    memset(&sa,0,sizeof(sa)); sa.sin_family=AF_INET; sa.sin_port=htons((unsigned short)rs->port); sa.sin_addr=rs->hostAddr;
    if (radio_is_stopping(rs)) { close_current_socket(rs); return -1; }
    cr=connect(rs->sock,(struct sockaddr*)&sa,sizeof(sa));
    if(cr<0 && radio_wait_connected(rs,&sa)!=0){ close_current_socket(rs); set_error(rs,"TCP connect failed"); RADIO_OPEN_DEBUG_PRINTF(("radio-open: connect failed to %s:%d\n", rs->host, rs->port)); return -1; }
    if (radio_is_stopping(rs)) { close_current_socket(rs); return -1; }
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    if (rs->isSSL) {
        if (radio_ssl_connect(rs) != 0) { close_current_socket(rs); return -1; }
        if (radio_is_stopping(rs)) { close_current_socket(rs); return -1; }
    }
#endif
    n=snprintf(req,sizeof(req),"GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: MiniAMP3/experimental\r\nIcy-MetaData: 1\r\nConnection: close\r\n\r\n",rs->path,rs->host);
    if(radio_send_all(rs,req,n)!=0){ close_current_socket(rs); set_error(rs, rs->isSSL ? "HTTPS read failed" : "cannot send HTTP request"); return -1; }
    reset_parser(rs);
    return 0;
}

static void close_current_socket(RadioStream *rs)
{
    if (!rs) return;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    radio_ssl_close_stream(rs);
#endif
    if (rs->sock != RADIO_INVALID_SOCKET) {
        RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: CloseSocket start fd=%ld\n", (long)rs->sock));
        rs->socket_close_count++;
        radio_close_socket(rs->sock);
        if (radio_open_socket_count > 0) radio_open_socket_count--;
        rs->sock = RADIO_INVALID_SOCKET;
        RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: CloseSocket done\n"));
        RADIO_STOP_DEBUG_PRINTF(("radio-stop: socket closed\n"));
    } else {
        RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: CloseSocket skipped fd=-1\n"));
    }
}

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

static void parse_headers(RadioStream *rs,char *h){ char *line=strtok(h,"\r\n"); int code=0; if(line && ci_starts(line,"ICY")) code=200; else if(line && ci_starts(line,"HTTP/")) sscanf(line,"HTTP/%*s %d",&code); else { set_error(rs,"invalid HTTP stream response"); RADIO_OPEN_DEBUG_PRINTF(("radio-open: HTTP header failed\n")); return; } if(code<200||code>299){ char msg[64]; sprintf(msg,"HTTP %d stream error",code); set_error(rs,msg); RADIO_OPEN_DEBUG_PRINTF(("radio-open: HTTP header failed status %d\n", code)); return; } while((line=strtok(NULL,"\r\n"))){ char *v=strchr(line,':'); if(!v) continue; *v++=0; line=trim(line); v=trim(v); if(ci_equals(line,"Content-Type")) radio_copy_string(rs->contentType,sizeof(rs->contentType),v); else if(ci_equals(line,"icy-metaint")){ rs->metaint=atoi(v); rs->audioUntilMeta=rs->metaint; } else if(ci_equals(line,"icy-br")) rs->bitrate=atoi(v); else if(ci_equals(line,"icy-name")) radio_copy_string(rs->stationName,sizeof(rs->stationName),v); else if(ci_equals(line,"icy-genre")) radio_copy_string(rs->genre,sizeof(rs->genre),v); else if(ci_equals(line,"icy-url")) radio_copy_string(rs->streamUrl,sizeof(rs->streamUrl),v); } if(rs->contentType[0] && (ci_starts(rs->contentType,"application/vnd.apple.mpegurl") || ci_starts(rs->contentType,"application/x-mpegurl"))) { set_error(rs,"HLS stream not supported"); RADIO_OPEN_DEBUG_PRINTF(("radio-open: HLS content type unsupported: %s\n", rs->contentType)); return; } RADIO_OPEN_DEBUG_PRINTF(("radio-open: final URL=%s content-type=%s URL codec hint=%s final selected codec=%s\n",
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
    end = m + n;
    for (i = 0; i + keyLen <= n; i++) {
        if (!memcmp(m + i, key, (size_t)keyLen)) {
            p = m + i + keyLen;
            for (i = 0; p + i < end && p[i] != '\''; i++)
                ;
            radio_copy_string(oldTitle, sizeof(oldTitle), rs->title);
            radio_copy_bytes(rs->title, sizeof(rs->title), p, i);
            if (strcmp(oldTitle, rs->title) != 0)
                printf("radio-resource: session=%lu ICY metadata updated (fixed buffer, active_icy_metadata_count=%ld)\n", rs->session_id, radio_active_icy_metadata_count);
            break;
        }
    }
}

static int process_bytes(RadioStream *rs, const unsigned char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (rs->parseState == RADIO_PARSE_HEADER) {
            if (rs->headerLen >= RADIO_HEADER_MAX - 1) { set_error(rs,"HTTP header too large"); return -1; }
            rs->header[rs->headerLen++] = (char)b[i]; rs->header[rs->headerLen] = 0;
            if (rs->headerLen >= 4 && !memcmp(rs->header + rs->headerLen - 4, "\r\n\r\n", 4)) {
                printf("radio-http-diag: session=%lu raw HTTP response header (%d bytes) follows:\n%s--- end of header ---\n", rs->session_id, rs->headerLen, rs->header);
                rs->headerDone = 1; parse_headers(rs, rs->header); if (rs->status == RADIO_STATUS_ERROR) return -1; rs->parseState = RADIO_PARSE_AUDIO;
            }
            continue;
        }
        if (rs->metaint > 0 && rs->parseState == RADIO_PARSE_AUDIO && rs->audioUntilMeta == 0) rs->parseState = RADIO_PARSE_META_LEN;
        if (rs->parseState == RADIO_PARSE_META_LEN) {
            rs->metaLen = b[i] * 16; rs->metaGot = 0; rs->metaLeft = rs->metaLen;
            rs->parseState = rs->metaLen ? RADIO_PARSE_META_PAYLOAD : RADIO_PARSE_AUDIO;
            if (!rs->metaLen) rs->audioUntilMeta = rs->metaint;
            continue;
        }
        if (rs->parseState == RADIO_PARSE_META_PAYLOAD) {
            if (rs->metaGot < RADIO_META_MAX) rs->meta[rs->metaGot++] = b[i];
            rs->metaLeft--;
            if (rs->metaLeft <= 0) { if (rs->metaGot > 0) parse_meta(rs, rs->meta, rs->metaGot); rs->audioUntilMeta = rs->metaint; rs->parseState = RADIO_PARSE_AUDIO; }
            continue;
        }
        if (rs->parseState == RADIO_PARSE_AUDIO) {
            ring_write(rs, &b[i], 1);
            if (rs->metaint > 0 && rs->audioUntilMeta > 0) rs->audioUntilMeta--;
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
    if (!radio_atexit_registered) { atexit(radio_app_exit_report); radio_atexit_registered = 1; }
    radio_reset_session_state(rs);
    rs->session_id = radio_next_session_id++;
    if (haveHostAddr) {
        memcpy(&rs->hostAddr, &hostAddrBe, sizeof(rs->hostAddr));
        rs->haveHostAddr = 1;
    }
    radio_active_stream_sessions++;
    radio_active_stream_tasks++;
    radio_active_decoder_count++;
    printf("radio-resource: session=%lu stream session/task/decoder allocated active_stream_sessions=%ld active_stream_tasks=%ld active_decoder_count=%ld\n", rs->session_id, radio_active_stream_sessions, radio_active_stream_tasks, radio_active_decoder_count);
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    printf("radio-ssl-diag: Radio_Open session=%lu url=\"%s\" inherited AmiSSL state: initialized=%d base=%p ext=%p master=%p ssl_count=%ld ssl_ctx_count=%ld\n",
        rs->session_id, url ? url : "(null)", radio_amissl_initialized, (void *)AmiSSLBase, (void *)AmiSSLExtBase, (void *)AmiSSLMasterBase, radio_active_ssl_count, radio_active_ssl_ctx_count);
#endif
    radio_debug_mem_report(rs->session_id, "before stream start");
    rs->status = RADIO_STATUS_CONNECTING;
    rs->size = RADIO_RING_BYTES;
    rs->ringAlloc = (unsigned char *)malloc(rs->size + 2UL * RADIO_RING_GUARD_BYTES);
    if (rs->ringAlloc) { rs->ring = rs->ringAlloc + RADIO_RING_GUARD_BYTES; radio_ring_set_canary(rs); }
    if (rs->ring) { radio_active_stream_buffer_count++; radio_active_audio_buffer_count++; printf("radio-resource: session=%lu stream/audio buffer allocated active_stream_buffer_count=%ld active_audio_buffer_count=%ld\n", rs->session_id, radio_active_stream_buffer_count, radio_active_audio_buffer_count); }
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
        radio_debug_mem_report(rs->session_id, "after stream started");
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
void Radio_RequestStop(RadioStream *rs){ if(!rs)return; radio_debug_mem_report(rs->session_id, "before stop"); rs->stop_request_count++; RADIO_STOP_DEBUG_PRINTF(("radio-stop: session=%lu stop requested count=%u status=%d fd=%ld\n", rs->session_id, rs->stop_request_count, (int)rs->status, (long)rs->sock)); if(rs->status==RADIO_STATUS_CLOSED)return; rs->stopping=1; rs->reconnectAttempts=RADIO_RECONNECT_MAX; rs->reconnectDelay=0; rs->status=RADIO_STATUS_STOPPING; close_current_socket(rs); RADIO_STOP_DEBUG_PRINTF(("radio-stop: marked stopping\n")); }
void Radio_Close(RadioStream *rs)
{
    if (!rs) return;
    printf("radio-http-diag: Radio_Close session=%lu status=%d everPlayed=%d headerDone=%d firstData=%d used=%lu metaint=%d reconnectAttempts=%d startPumps=%d stopping=%d stopReq=%u contentType=\"%s\" host=\"%s\" path=\"%s\" error=\"%s\"\n",
        rs->session_id, (int)rs->status, rs->everPlayed, rs->headerDone, rs->firstDataLogged, rs->used, rs->metaint,
        rs->reconnectAttempts, rs->startPumps, rs->stopping, rs->stop_request_count,
        rs->contentType, rs->host, rs->path, rs->error);
    RADIO_STOP_DEBUG_PRINTF(("radio-stop: Radio_Close entered session=%lu\n", rs->session_id));
    rs->cleanup_count++;
    if (rs->cleanup_count > 1) radio_duplicate_cleanup_warning(rs, "session cleanup", rs->cleanup_count);
    Radio_RequestStop(rs);
    close_current_socket(rs);
    rs->status = RADIO_STATUS_CLOSED;
    rs->stream_buffer_free_count++;
    rs->audio_buffer_free_count++;
    if (rs->stream_buffer_free_count > 1) radio_duplicate_cleanup_warning(rs, "stream buffer free", rs->stream_buffer_free_count);
    else { if (rs->ringAlloc) { radio_ring_check_canary(rs, "before cleanup"); free(rs->ringAlloc); if (radio_active_stream_buffer_count > 0) radio_active_stream_buffer_count--; } }
    if (rs->audio_buffer_free_count > 1) radio_duplicate_cleanup_warning(rs, "audio buffer free", rs->audio_buffer_free_count);
    else { if (radio_active_audio_buffer_count > 0) radio_active_audio_buffer_count--; }
    rs->decoder_free_count++;
    if (rs->decoder_free_count > 1) radio_duplicate_cleanup_warning(rs, "decoder free", rs->decoder_free_count);
    else { if (radio_active_decoder_count > 0) radio_active_decoder_count--; }
    rs->ring = NULL; rs->ringAlloc = NULL;
    rs->size = rs->used = rs->rpos = rs->wpos = 0;
    rs->task_exit_count++;
    if (radio_active_stream_tasks > 0) radio_active_stream_tasks--;
    if (radio_active_stream_sessions > 0) radio_active_stream_sessions--;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    rs->amissl_cleanup_count++;
    radio_ssl_global_cleanup();
#endif
    radio_debug_mem_report(rs->session_id, "after stop cleanup");
    radio_resource_summary(rs, "after stop cleanup");
    RADIO_STOP_DEBUG_PRINTF(("radio-stop: stream task exiting / Radio_Close exited session=%lu task_exit_count=%u cleanup_count=%u stop_request_count=%u ssl_free_count=%u socket_close_count=%u decoder_free_count=%u\n", rs->session_id, rs->task_exit_count, rs->cleanup_count, rs->stop_request_count, rs->ssl_free_count, rs->socket_close_count, rs->decoder_free_count));
    free(rs);
}

/* Release the process-wide network libraries exactly once, at application exit.
 *
 * SocketBase (bsdsocket.library) and the AmiSSL master library are opened lazily
 * and shared across the probe, the radio_browser search and every playback
 * child; the per-session code only ever opened them (and, for AmiSSL, kept the
 * master open for the program's lifetime so repeated InitAmiSSLMaster() could
 * not relock HTTPS).  Nothing ever *closed* them, so when the app quit it left
 * bsdsocket.library open with a reference held by a now-dead task; the TCP stack
 * then kept stale per-task socket state and the next launch of the app could no
 * longer open a working socket ("Search failed" even though the network is up).
 * Closing them here, in reverse open order (AmiSSL first, since it was handed
 * SocketBase), lets the stack reclaim everything cleanly.  Must run on the main
 * task after every playback child has been stopped and reaped. */
void Radio_NetworkShutdown(void)
{
#if defined(AMIGA_M68K)
#if defined(HAVE_AMISSL)
    radio_ssl_global_cleanup();           /* closes any still-open per-task AmiSSL */
    if (AmiSSLMasterBase) {
        CloseLibrary(AmiSSLMasterBase);
        AmiSSLMasterBase = NULL;
        radio_amissl_initialized = 0;
    }
#endif
    if (SocketBase) {
        CloseLibrary(SocketBase);
        SocketBase = NULL;
    }
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
        n = (int)SSL_read(rs->ssl, (char *)b, sizeof(b));
        if (n < 0) {
            int e = SSL_get_error(rs->ssl, n);
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) wb = 1;
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
        if (!rs->headerDone) { set_error(rs, rs->isSSL ? "HTTPS read failed" : "HTTP header read failed"); RADIO_OPEN_DEBUG_PRINTF(("radio-open: HTTP header failed\n")); return -1; }
        if (!rs->everPlayed) { set_error(rs, "radio stream ended before audio buffered"); return -1; }
        rs->reconnectDelay = RADIO_RECONNECT_BACKOFF_PUMPS;
        set_status(rs, RADIO_STATUS_RECONNECTING);
        return 0;
    }
    rs->zeroBytePumps = 0;
    if (!rs->firstDataLogged) {
        printf("radio-stream: first data received fd=%ld ssl=%p\n",
            (long)rs->sock,
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
            (void *)rs->ssl
#else
            (void *)0
#endif
        );
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
        if (!rs->everPlayed) printf("radio-stream: first decoder frame / playback buffer ready fd=%ld\n", (long)rs->sock);
        rs->reconnectAttempts = 0; rs->reconnectDelay = 0; rs->everPlayed = 1; set_status(rs, RADIO_STATUS_PLAYING);
    } else if (rs->headerDone && rs->status != RADIO_STATUS_PLAYING)
        set_status(rs, RADIO_STATUS_BUFFERING);
    return n;
}
int Radio_ReadAudio(RadioStream *rs,unsigned char *buf,int maxBytes){ int got; if(!rs||!buf||maxBytes<=0)return 0; if(radio_is_stopping(rs)) return 0; while(!radio_is_stopping(rs) && rs->status!=RADIO_STATUS_PLAYING && rs->used<RADIO_START_THRESHOLD && rs->status!=RADIO_STATUS_ERROR) { if(Radio_Pump(rs)<=0 && !rs->everPlayed && (++rs->zeroBytePumps>=RADIO_ZERO_BYTE_PUMP_MAX || radio_note_start_wait(rs,"radio stream did not buffer audio")<0)) { if(rs->status!=RADIO_STATUS_ERROR) set_error(rs,"radio stream did not buffer audio"); break; } } while(!radio_is_stopping(rs) && rs->used==0 && rs->status!=RADIO_STATUS_ERROR) { if(Radio_Pump(rs)<=0 && !rs->everPlayed && (++rs->zeroBytePumps>=RADIO_ZERO_BYTE_PUMP_MAX || radio_note_start_wait(rs,"radio stream did not deliver audio")<0)) { if(rs->status!=RADIO_STATUS_ERROR) set_error(rs,"radio stream did not deliver audio"); break; } } if(radio_is_stopping(rs)) return 0; got=ring_read(rs,buf,maxBytes); if(rs->status==RADIO_STATUS_PLAYING && rs->used<RADIO_LOW_WATER_BYTES) set_status(rs,RADIO_STATUS_BUFFERING); if(rs->status==RADIO_STATUS_BUFFERING && rs->used>=RADIO_START_THRESHOLD) set_status(rs,RADIO_STATUS_PLAYING); return got; }
int Radio_ReadStartupAudio(RadioStream *rs,unsigned char *buf,int maxBytes,unsigned long timeoutMs){ clock_t start; int got; if(!rs||!buf||maxBytes<=0)return 0; start=clock(); while(!radio_is_stopping(rs)&&rs->used==0&&rs->status!=RADIO_STATUS_ERROR){ if(Radio_Pump(rs)<0)break; if(timeoutMs>0 && (unsigned long)((clock()-start)*1000UL/CLOCKS_PER_SEC)>=timeoutMs){ set_error(rs,"AAC stream start timeout"); close_current_socket(rs); break; } } if(radio_is_stopping(rs)) return 0; got=ring_read(rs,buf,maxBytes); if(rs->headerDone&&rs->status!=RADIO_STATUS_PLAYING&&rs->status!=RADIO_STATUS_ERROR) set_status(rs,RADIO_STATUS_BUFFERING); return got; }
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
