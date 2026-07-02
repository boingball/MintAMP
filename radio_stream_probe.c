/*
 * Small fixed-buffer HTTP/ICY stream probe.
 *
 * Test harness build:
 *   gcc -std=gnu89 -Wall -Wextra -DRB_STREAM_PROBE_TEST radio_stream_probe.c -o /tmp/rb_stream_probe_test
 */

#include "radio_stream_probe.h"
#include "radio_debug.h"
#include "radio_stream.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "miniamp_memguard.h"

#if defined(AMIGA_M68K)
#include <exec/types.h>
#include <exec/libraries.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/bsdsocket.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#ifndef RB_STREAM_PROBE_EXTERNAL_SOCKETBASE
struct Library *SocketBase __attribute__((weak));
#endif
#define RB_PROBE_SOCKET long
#define RB_PROBE_INVALID_SOCKET (-1)
#define rb_probe_close_socket(s) CloseSocket(s)
#if defined(HAVE_AMISSL)
#include <libraries/amisslmaster.h>
#include <proto/amisslmaster.h>
#include <proto/amissl.h>
#include <amissl/amissl.h>
#include <errno.h>
/* Weak so this module can link standalone (test harness) or beside
 * radio_stream.c which provides the strong definitions. */
struct Library *AmiSSLMasterBase __attribute__((weak));
struct Library *AmiSSLBase __attribute__((weak));
struct Library *AmiSSLExtBase __attribute__((weak));
static int rb_probe_amissl_initialized = 0;
/* Cumulative per-task InitAmiSSL()/CleanupAmiSSL() debug counters for this
 * (parent/GUI) task's probe/favicon fetches. */
static long rb_probe_amissl_init_count = 0;
static long rb_probe_amissl_cleanup_count = 0;
#endif /* HAVE_AMISSL */
#else
#include <unistd.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#define RB_PROBE_SOCKET int
#define RB_PROBE_INVALID_SOCKET (-1)
#define rb_probe_close_socket(s) close(s)
#endif


/* AvailMem() bracketing around SSL_free()/SSL_CTX_free() specifically (see
 * the matching radio_debug_mem_report() in radio_stream.c) -- narrows any
 * memory movement on a failed probe TLS handshake down to exactly those two
 * AmiSSL calls instead of the whole probe. */
static void rb_probe_debug_mem_report(unsigned long session_id, const char *where)
{
#if defined(AMIGA_M68K)
    RADIO_DBG(printf("rb-probe-mem: session=%lu %s AvailMem(any)=%lu fast=%lu chip=%lu\n",
        session_id, where ? where : "", (unsigned long)AvailMem(MEMF_ANY),
        (unsigned long)AvailMem(MEMF_FAST), (unsigned long)AvailMem(MEMF_CHIP)));
#else
    RADIO_DBG(printf("rb-probe-mem: session=%lu %s AvailMem(any)=n/a fast=n/a chip=n/a\n",
        session_id, where ? where : ""));
#endif
}

static void rb_probe_format_ipv4_be(unsigned long addr_be, char *out, int out_size)
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

#define RB_PROBE_DEFAULT_PORT 80
#define RB_PROBE_MAX_HOST 256
#define RB_PROBE_MAX_PATH 512
#define RB_PROBE_MAX_REQUEST 1024
#define RB_PROBE_HEADER_BUF 4096
#define RB_PROBE_READ_CHUNK 512
#define RB_PROBE_MAX_REDIRECTS 3
#define RB_PROBE_MAX_URL 512

static unsigned long rb_probe_next_session_id = 1;
static long rb_probe_open_socket_count = 0;

#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
static void rb_probe_backoff_sleep(void)
{
    Delay(2); /* ~40ms (2 ticks @ 50Hz), same budget as radio_stream.c's handshake poll */
}
#endif

#if defined(AMIGA_M68K) && !defined(RB_STREAM_PROBE_EXTERNAL_SOCKETBASE)
/* bsdsocket.library is now opened once by Radio_NetworkInit() at app startup
 * and closed once by Radio_NetworkShutdown() at app exit (see radio_stream.c)
 * instead of per-probe -- this is now a deliberate no-op, kept so its call
 * sites below don't all need to be removed individually. */
static void rb_probe_release_idle_socketbase(void)
{
    (void)rb_probe_open_socket_count;
}
#endif

/* Transport close mode: GRACEFUL attempts SSL_shutdown() (only for a clean,
 * successful 2xx probe/fetch); ABORT always skips it.  301/redirects,
 * non-2xx statuses, TLS/recv/server-close errors must use ABORT -- calling
 * SSL_shutdown() on a session in one of those states is what produces
 * recoverable AmiSSL alerts. */
typedef enum {
    RB_PROBE_CLOSE_GRACEFUL = 0,
    RB_PROBE_CLOSE_ABORT = 1
} RbProbeCloseMode;
static const char *rb_probe_close_mode_name(RbProbeCloseMode mode) { return mode == RB_PROBE_CLOSE_GRACEFUL ? "graceful" : "abort"; }

typedef struct RbProbeUrl {
    char host[RB_PROBE_MAX_HOST];
    char path[RB_PROBE_MAX_PATH];
    int port;
    int isSSL;
} RbProbeUrl;

typedef struct RbProbeTransport {
    RB_PROBE_SOCKET sock;
    int isSSL;
    unsigned long session_id;
    char host[RB_PROBE_MAX_HOST];
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    SSL *ssl;
    SSL_CTX *ctx;
    int sslHandshakeDone;
    int sslStatePoisoned;
#endif
} RbProbeTransport;

#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
/* SSL_read() can legitimately need several calls (WANT_READ/WANT_WRITE) even
 * on a nominally blocking socket -- same reasoning as the SSL_connect() retry
 * in rb_probe_transport_open().  dst_cap and fill are only for the debug
 * print (the buffer capacity and bytes already filled at the call site). */
static int rb_probe_ssl_read_retrying(RbProbeTransport *transport, char *dst, int want, int dst_cap, int fill)
{
    int n;
    int tries;

    for (tries = 0; tries < 150; tries++) {
        int ssl_err;

        n = (int)SSL_read(transport->ssl, dst, want);
        RADIO_DBG(printf("rb-probe-ssl-read: session=%lu ssl=%p ctx=%p fd=%ld dst=%p dst_cap=%d requested=%d returned=%d fill=%d ring_free=0\n",
            transport->session_id, (void *)transport->ssl, (void *)transport->ctx, (long)transport->sock, (void *)dst, dst_cap, want, n, fill));
        if (n > 0) return n;
        ssl_err = SSL_get_error(transport->ssl, n);
        RADIO_DBG(printf("rb-probe-ssl-read: session=%lu SSL_get_error=%d ret=%d fd=%ld\n", transport->session_id, ssl_err, n, (long)transport->sock));
        if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
            rb_probe_backoff_sleep();
            continue;
        }
        {
            unsigned long ssl_lib_error = ERR_get_error();
            char ssl_error_buf[160];
            ssl_error_buf[0] = '\0';
            if (ssl_lib_error != 0)
                ERR_error_string_n(ssl_lib_error, ssl_error_buf, sizeof(ssl_error_buf));
            RADIO_DBG(printf("rb-probe-ssl-read: session=%lu read failed ssl_error=%d lib_error=%08lx (%s)\n",
                transport->session_id, ssl_err, ssl_lib_error, ssl_error_buf[0] ? ssl_error_buf : "none"));
            if (ssl_err == SSL_ERROR_SSL || ssl_lib_error != 0) {
                /* Survivable now: the probe fails cleanly, its SSL objects
                 * are freed normally, and HTTPS stays enabled -- see the
                 * matching SSL_read fault handling in radio_stream.c's
                 * Radio_Pump(). Only the repeated-fault fuse hard-poisons. */
                Radio_ReportTlsFault(ssl_err == SSL_ERROR_SSL ? "probe SSL_ERROR_SSL from SSL_read" : "probe fatal AmiSSL error queue from SSL_read");
                ERR_clear_error();
                if (Radio_IsTlsPoisoned()) {
                    transport->sslStatePoisoned = 1;
                    RADIO_DBG(printf("rb-probe-ssl-read: session=%lu SSL state marked poisoned (fault limit reached) -- will skip SSL_free/SSL_CTX_free/CleanupAmiSSL on close\n", transport->session_id));
                }
            }
        }
        return n;
    }
    return n;
}
#endif

static int rb_probe_ascii_starts_nocase(const char *s, const char *prefix)
{
    unsigned char cs;
    unsigned char cp;

    if (!s || !prefix) return 0;
    while (*prefix) {
        if (!*s) return 0;
        cs = (unsigned char)*s;
        cp = (unsigned char)*prefix;
        if (tolower(cs) != tolower(cp)) return 0;
        s++;
        prefix++;
    }
    return 1;
}

static void rb_probe_copy_trim(char *dst, int dst_size, const char *src, int len)
{
    int start;
    int end;
    int n;

    if (!dst || dst_size <= 0) return;
    dst[0] = '\0';
    if (!src || len <= 0) return;
    start = 0;
    end = len;
    while (start < end && (src[start] == ' ' || src[start] == '\t')) start++;
    while (end > start && (src[end - 1] == ' ' || src[end - 1] == '\t' ||
                           src[end - 1] == '\r' || src[end - 1] == '\n')) end--;
    n = end - start;
    if (n > dst_size - 1) n = dst_size - 1;
    if (n > 0) memcpy(dst, src + start, (size_t)n);
    dst[n] = '\0';
}


static long rb_probe_sock_errno(void)
{
#if defined(AMIGA_M68K)
    return Errno();
#else
    return errno;
#endif
}

static long rb_probe_ioerr_value(void)
{
#if defined(AMIGA_M68K)
    return IoErr();
#else
    return 0;
#endif
}

static void rb_probe_socket_fail(const RbProbeTransport *transport, const char *where)
{
    RADIO_DBG(printf("radio-socket: probe socket failed session=%lu host=%s fd=-1 errno=%ld IoErr=%ld open_socket_count=%ld probe_open_socket_count=%ld where=%s\n",
        transport ? transport->session_id : 0, transport ? transport->host : "",
        rb_probe_sock_errno(), rb_probe_ioerr_value(), rb_probe_open_socket_count,
        rb_probe_open_socket_count, where ? where : "");)
}

static void rb_probe_info_init(RbStreamInfo *info)
{
    if (!info) return;
    memset(info, 0, sizeof(*info));
    info->codec = RB_STREAM_CODEC_UNKNOWN;
}

static int rb_probe_copy_string(char *dst, int dst_size, const char *src)
{
    int len;

    if (!dst || dst_size <= 0 || !src) return RB_STREAM_PROBE_ERR_BAD_ARG;
    len = (int)strlen(src);
    if (len >= dst_size) return RB_STREAM_PROBE_ERR_URL_TOO_LONG;
    memcpy(dst, src, (size_t)len + 1);
    return RB_STREAM_PROBE_OK;
}

static int rb_probe_set_final_url(RbStreamInfo *info, const char *url)
{
    int len;

    if (!info || !url) return RB_STREAM_PROBE_ERR_BAD_ARG;
    len = (int)strlen(url);
    if (len >= (int)sizeof(info->final_url)) {
        info->final_url[0] = '\0';
        return RB_STREAM_PROBE_ERR_URL_TOO_LONG;
    }
    memcpy(info->final_url, url, (size_t)len + 1);
    return RB_STREAM_PROBE_OK;
}

static int rb_probe_parse_url(const char *url, RbProbeUrl *parsed)
{
    const char *p;
    const char *slash;
    const char *colon;
    const char *host_start;
    int host_len;
    int path_len;
    int port;

    if (!url || !parsed) return RB_STREAM_PROBE_ERR_BAD_ARG;
    memset(parsed, 0, sizeof(*parsed));
    parsed->port = RB_PROBE_DEFAULT_PORT;

#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    if (rb_probe_ascii_starts_nocase(url, "https://")) {
        parsed->isSSL = 1;
        parsed->port = 443;
        host_start = url + 8;
    } else if (rb_probe_ascii_starts_nocase(url, "http://")) {
        host_start = url + 7;
    } else {
        return RB_STREAM_PROBE_ERR_BAD_URL;
    }
#else
    if (rb_probe_ascii_starts_nocase(url, "https://"))
        return RB_STREAM_PROBE_ERR_UNSUPPORTED_TLS;
    if (!rb_probe_ascii_starts_nocase(url, "http://"))
        return RB_STREAM_PROBE_ERR_BAD_URL;
    host_start = url + 7;
#endif
    if (!*host_start) return RB_STREAM_PROBE_ERR_BAD_URL;
    slash = strchr(host_start, '/');
    if (!slash) slash = host_start + strlen(host_start);
    if (slash == host_start) return RB_STREAM_PROBE_ERR_BAD_URL;

    colon = NULL;
    p = host_start;
    while (p < slash) {
        if (*p == ':') colon = p;
        p++;
    }

    if (colon) {
        host_len = (int)(colon - host_start);
        if (host_len <= 0) return RB_STREAM_PROBE_ERR_BAD_URL;
        port = 0;
        p = colon + 1;
        if (p >= slash) return RB_STREAM_PROBE_ERR_BAD_URL;
        while (p < slash) {
            if (!isdigit((unsigned char)*p)) return RB_STREAM_PROBE_ERR_BAD_URL;
            port = port * 10 + (*p - '0');
            if (port > 65535) return RB_STREAM_PROBE_ERR_BAD_URL;
            p++;
        }
        if (port <= 0) return RB_STREAM_PROBE_ERR_BAD_URL;
        parsed->port = port;
    } else {
        host_len = (int)(slash - host_start);
    }

    if (host_len >= RB_PROBE_MAX_HOST) return RB_STREAM_PROBE_ERR_URL_TOO_LONG;
    memcpy(parsed->host, host_start, (size_t)host_len);
    parsed->host[host_len] = '\0';

    if (*slash) {
        path_len = (int)strlen(slash);
        if (path_len >= RB_PROBE_MAX_PATH) return RB_STREAM_PROBE_ERR_URL_TOO_LONG;
        memcpy(parsed->path, slash, (size_t)path_len + 1);
    } else {
        strcpy(parsed->path, "/");
    }
    return RB_STREAM_PROBE_OK;
}

static int rb_probe_append(char *out, int out_size, int *pos, const char *text)
{
    if (!out || !pos || out_size <= 0 || !text) return RB_STREAM_PROBE_ERR_BAD_ARG;
    while (*text) {
        if (*pos >= out_size - 1) return RB_STREAM_PROBE_ERR_REQUEST_TOO_BIG;
        out[*pos] = *text;
        (*pos)++;
        out[*pos] = '\0';
        text++;
    }
    return RB_STREAM_PROBE_OK;
}

static int rb_probe_append_url(char *out, int out_size, int *pos, const char *text)
{
    int rc;

    rc = rb_probe_append(out, out_size, pos, text);
    if (rc == RB_STREAM_PROBE_ERR_REQUEST_TOO_BIG) return RB_STREAM_PROBE_ERR_URL_TOO_LONG;
    return rc;
}

static int rb_probe_build_request(char *out, int out_size, const RbProbeUrl *url)
{
    int pos;
    int rc;

    if (!out || out_size <= 0 || !url) return RB_STREAM_PROBE_ERR_BAD_ARG;
    out[0] = '\0';
    pos = 0;
    rc = rb_probe_append(out, out_size, &pos, "GET ");
    if (rc < 0) return rc;
    rc = rb_probe_append(out, out_size, &pos, url->path);
    if (rc < 0) return rc;
    rc = rb_probe_append(out, out_size, &pos, " HTTP/1.1\r\nHost: ");
    if (rc < 0) return rc;
    rc = rb_probe_append(out, out_size, &pos, url->host);
    if (rc < 0) return rc;
    return rb_probe_append(out, out_size, &pos,
        "\r\nUser-Agent: BoingPlayer/0.1 AmigaOS\r\n"
        "Icy-MetaData: 1\r\n"
        "Connection: close\r\n\r\n");
}

static int rb_probe_resolve_location(const RbProbeUrl *base, const char *location,
                                     char *out, int out_size)
{
    int pos;
    int rc;
    const char *last_slash;
    int dir_len;
    char port_buf[16];

    if (!base || !location || !out || out_size <= 0) return RB_STREAM_PROBE_ERR_BAD_ARG;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    if (rb_probe_ascii_starts_nocase(location, "https://") ||
        rb_probe_ascii_starts_nocase(location, "http://"))
        return rb_probe_copy_string(out, out_size, location);
#else
    if (rb_probe_ascii_starts_nocase(location, "https://"))
        return RB_STREAM_PROBE_ERR_UNSUPPORTED_TLS;
    if (rb_probe_ascii_starts_nocase(location, "http://"))
        return rb_probe_copy_string(out, out_size, location);
#endif
    if (location[0] == '/') {
        out[0] = '\0';
        pos = 0;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
        rc = rb_probe_append_url(out, out_size, &pos, base->isSSL ? "https://" : "http://");
#else
        rc = rb_probe_append_url(out, out_size, &pos, "http://");
#endif
        if (rc < 0) return rc;
        rc = rb_probe_append_url(out, out_size, &pos, base->host);
        if (rc < 0) return rc;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
        if (base->port != (base->isSSL ? 443 : RB_PROBE_DEFAULT_PORT)) {
#else
        if (base->port != RB_PROBE_DEFAULT_PORT) {
#endif
            sprintf(port_buf, ":%d", base->port);
            rc = rb_probe_append_url(out, out_size, &pos, port_buf);
            if (rc < 0) return rc;
        }
        return rb_probe_append_url(out, out_size, &pos, location);
    }
    if (location[0] == '\0') return RB_STREAM_PROBE_ERR_BAD_URL;

    out[0] = '\0';
    pos = 0;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    rc = rb_probe_append_url(out, out_size, &pos, base->isSSL ? "https://" : "http://");
#else
    rc = rb_probe_append_url(out, out_size, &pos, "http://");
#endif
    if (rc < 0) return rc;
    rc = rb_probe_append_url(out, out_size, &pos, base->host);
    if (rc < 0) return rc;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    if (base->port != (base->isSSL ? 443 : RB_PROBE_DEFAULT_PORT)) {
#else
    if (base->port != RB_PROBE_DEFAULT_PORT) {
#endif
        sprintf(port_buf, ":%d", base->port);
        rc = rb_probe_append_url(out, out_size, &pos, port_buf);
        if (rc < 0) return rc;
    }
    last_slash = strrchr(base->path, '/');
    dir_len = last_slash ? (int)(last_slash - base->path) + 1 : 1;
    if (pos + dir_len >= out_size) return RB_STREAM_PROBE_ERR_URL_TOO_LONG;
    memcpy(out + pos, base->path, (size_t)dir_len);
    pos += dir_len;
    out[pos] = '\0';
    return rb_probe_append_url(out, out_size, &pos, location);
}

#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
static int rb_probe_ensure_amissl(void)
{
    if (Radio_IsTlsPoisoned()) {
        /* AmiSSL cleanup has already been skipped/leaked for this run --
         * never call back into it (callers also refuse HTTPS up front; this
         * is the belt-and-braces check on the transport path itself). */
        return -1;
    }
    if (!SocketBase) {
        /* Probes run in the same parent/GUI task that ran Radio_NetworkInit()
         * -- adopt its bsdsocket.library rather than opening a second one.
         * (This file's weak SocketBase does not reliably merge with
         * radio_stream.c's strong definition under the m68k hunk linker, so
         * the value has to come through a function call.) */
        void *shared_socket = NULL;
        Radio_GetNetworkBases(&shared_socket, NULL, NULL);
        if (shared_socket) SocketBase = (struct Library *)shared_socket;
    }
    if (!SocketBase) {
        SocketBase = OpenLibrary("bsdsocket.library", 4);
        if (!SocketBase) return -1;
    }
    if (!AmiSSLBase) {
        /* Same story for the shared AmiSSL instance: adopt the one
         * Radio_NetworkInit()/radio_stream.c opened instead of running
         * OpenLibrary(amisslmaster)/InitAmiSSLMaster()/OpenAmiSSLTags() a
         * SECOND time in this task -- exactly that repeated open is what
         * raised a recoverable alert on the first Play and can wedge AmiSSL
         * outright. */
        void *shared_base = NULL, *shared_ext = NULL, *shared_master = NULL;
        Radio_GetAmiSslShared(&shared_base, &shared_ext, &shared_master);
        if (shared_base) {
            AmiSSLBase = (struct Library *)shared_base;
            AmiSSLExtBase = (struct Library *)shared_ext;
            AmiSSLMasterBase = (struct Library *)shared_master;
            RADIO_DBG(printf("radio-netinit: probe adopted shared AmiSSL instance base=%p ext=%p master=%p\n", (void *)AmiSSLBase, (void *)AmiSSLExtBase, (void *)AmiSSLMasterBase);)
        }
    }
    /* rb_probe_amissl_initialized (this file's own flag, NOT AmiSSLBase) is
     * "have I, this task, called InitAmiSSL() yet" -- AmiSSLBase now stays
     * open for the whole app's lifetime (see radio_stream.c's
     * Radio_NetworkInit()/Radio_NetworkShutdown()) instead of being opened
     * and closed on every probe. Checking AmiSSLBase itself here would be
     * wrong now: it stays non-NULL across every task, so it can no longer
     * answer "has *this* task called InitAmiSSL()" the way it used to when
     * the library was opened and closed per probe.
     *
     * Each task must still call its own InitAmiSSL(), paired with its own
     * CleanupAmiSSL() -- that part of the per-task contract is unchanged;
     * only the library-level open/close moved to app startup/shutdown. */
    if (rb_probe_amissl_initialized) return 0;
    if (!AmiSSLBase) {
        /* No shared instance to adopt either (Radio_NetworkInit() failed or
         * this is a standalone build without radio_stream.c) -- lazily open
         * our own here as a last resort. */
        RADIO_DBG(printf("radio-netinit: probe found no shared AmiSSL instance, opening its own\n");)
        if (!AmiSSLMasterBase) {
            AmiSSLMasterBase = OpenLibrary("amisslmaster.library", AMISSLMASTER_MIN_VERSION);
            if (!AmiSSLMasterBase) return -1;
            if (!InitAmiSSLMaster(AMISSL_CURRENT_VERSION, TRUE)) {
                CloseLibrary(AmiSSLMasterBase); AmiSSLMasterBase = NULL; return -1;
            }
        }
        if (OpenAmiSSLTags(AMISSL_CURRENT_VERSION,
                           AmiSSL_UsesOpenSSLStructs, TRUE,
                           AmiSSL_GetAmiSSLBase, (ULONG)&AmiSSLBase,
                           AmiSSL_GetAmiSSLExtBase, (ULONG)&AmiSSLExtBase,
                           AmiSSL_SocketBase, (ULONG)SocketBase,
                           AmiSSL_ErrNoPtr, (ULONG)&errno,
                           TAG_DONE) != 0)
            return -1;
    }
    /* Deliberately using AmiSSL's own auto-allocated timer.device port
     * (not AmiSSL_TimerPort with one of our own) -- see the matching comment
     * in radio_stream.c: a caller-supplied port was tried and reverted after
     * it correlated with the playback child going unresponsive to a stop
     * signal while mid-SSL_connect(). */
    if (InitAmiSSL(AmiSSL_SocketBase, (ULONG)SocketBase,
                   AmiSSL_ErrNoPtr, (ULONG)&errno,
                   TAG_DONE) != 0) {
        /* Do NOT CloseAmiSSL() here: this task may not be the one that
         * opened the shared library -- only Radio_NetworkShutdown()
         * closes it, once, at final app shutdown. */
        return -1;
    }
    rb_probe_amissl_initialized = 1;
    rb_probe_amissl_init_count++;
    RADIO_DBG(printf("radio-resource: probe AmiSSL init count=%ld cleanup count=%ld\n", rb_probe_amissl_init_count, rb_probe_amissl_cleanup_count);)
    return 0;
}

/* amisslmaster.library and the shared AmiSSLBase/AmiSSLExtBase instance are
 * both opened once by radio_stream.c's Radio_NetworkInit() (or lazily here,
 * whichever runs first) and closed once by Radio_NetworkShutdown() at app
 * exit. The per-task AmiSSL context (InitAmiSSL() in
 * rb_probe_ensure_amissl()) is a different story: it must still be cleaned
 * up by this (parent/GUI) task after every probe/favicon fetch via
 * CleanupAmiSSL(), so a fresh InitAmiSSL() can run again next time --
 * skipping that per-task pairing, not the shared library close, is what
 * crashes inside SSL_CTX_new()/SSL_new() in a later task. */
static void rb_probe_cleanup_amissl(void)
{
    RADIO_DBG(printf("radio-ssl-diag: probe cleanup ENTER probe_init=%d base=%p ext=%p master=%p\n", rb_probe_amissl_initialized, (void *)AmiSSLBase, (void *)AmiSSLExtBase, (void *)AmiSSLMasterBase);)
    if (rb_probe_amissl_initialized && Radio_IsTlsPoisoned()) {
        RADIO_DBG(printf("radio-cleanup: probe CleanupAmiSSL skipped (poisoned) leaking per-task AmiSSL state to avoid crashing on corrupted AmiSSL internals\n");)
        Radio_MarkTlsPoisoned("probe CleanupAmiSSL skipped for poisoned AmiSSL session");
        rb_probe_amissl_initialized = 0;
    } else if (rb_probe_amissl_initialized) {
        CleanupAmiSSL(TAG_DONE);
        rb_probe_amissl_initialized = 0;
        rb_probe_amissl_cleanup_count++;
        RADIO_DBG(printf("radio-resource: probe AmiSSL cleaned up init count=%ld cleanup count=%ld\n", rb_probe_amissl_init_count, rb_probe_amissl_cleanup_count);)
    }
    /* AmiSSLBase/AmiSSLExtBase are shared for the whole app's lifetime now
     * (see radio_stream.c's Radio_NetworkShutdown()) -- this function no
     * longer closes them itself. */
    RADIO_DBG(printf("radio-ssl-diag: probe cleanup EXIT  probe_init=%d base=%p ext=%p master=%p\n", rb_probe_amissl_initialized, (void *)AmiSSLBase, (void *)AmiSSLExtBase, (void *)AmiSSLMasterBase);)
}

#endif

static void rb_probe_transport_close_mode(RbProbeTransport *transport, RbProbeCloseMode mode, int http_status);
static void rb_probe_transport_close(RbProbeTransport *transport);

static int rb_probe_transport_open(RbProbeTransport *transport, const char *host, int port, int use_ssl, unsigned long *host_addr_be)
{
    struct hostent *he;
    struct sockaddr_in sa;

    if (!transport || !host) return RB_STREAM_PROBE_ERR_BAD_ARG;
    transport->sock = RB_PROBE_INVALID_SOCKET;
    transport->isSSL = 0;
    transport->session_id = rb_probe_next_session_id++;
    rb_probe_copy_trim(transport->host, (int)sizeof(transport->host), host, (int)strlen(host));
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    transport->ssl = NULL;
    transport->ctx = NULL;
    transport->sslHandshakeDone = 0;
    transport->sslStatePoisoned = 0;
#endif

#if defined(AMIGA_M68K) && !defined(RB_STREAM_PROBE_EXTERNAL_SOCKETBASE)
    if (!SocketBase) {
        /* Adopt the app-wide bsdsocket.library first (see
         * rb_probe_ensure_amissl(): weak SocketBase does not reliably merge
         * with radio_stream.c's strong definition). */
        void *shared_socket = NULL;
        Radio_GetNetworkBases(&shared_socket, NULL, NULL);
        if (shared_socket) SocketBase = (struct Library *)shared_socket;
    }
    if (!SocketBase) {
        SocketBase = OpenLibrary("bsdsocket.library", 4);
        if (!SocketBase) return RB_STREAM_PROBE_ERR_CONNECT;
    }
#endif
    he = gethostbyname(host);
    if (!he || !he->h_addr_list || !he->h_addr_list[0]) return RB_STREAM_PROBE_ERR_DNS;
    if (host_addr_be) {
        char addr_text[16];
        memset(host_addr_be, 0, sizeof(*host_addr_be));
        memcpy(host_addr_be, he->h_addr_list[0], 4);
        rb_probe_format_ipv4_be(*host_addr_be, addr_text, (int)sizeof(addr_text));
        RADIO_DBG(printf("rb-probe DNS: resolved %s -> %s\n", host, addr_text);)
    }
    transport->sock = socket(AF_INET, SOCK_STREAM, 0);
    if (transport->sock == RB_PROBE_INVALID_SOCKET) { rb_probe_socket_fail(transport, "socket"); return RB_STREAM_PROBE_ERR_CONNECT; }
    rb_probe_open_socket_count++;
    RADIO_DBG(printf("radio-socket: probe socket opened session=%lu host=%s fd=%ld open_socket_count=%ld probe_open_socket_count=%ld\n", transport->session_id, transport->host, (long)transport->sock, rb_probe_open_socket_count, rb_probe_open_socket_count);)
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    memcpy(&sa.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
    if (connect(transport->sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        rb_probe_transport_close(transport);
        return RB_STREAM_PROBE_ERR_CONNECT;
    }
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    if (use_ssl) {
        int ssl_connect_rc;
        int ssl_error;
        unsigned long ssl_lib_error;
        char ssl_error_buf[160];
        int sni_set;
        int tries;

        if (Radio_IsTlsPoisoned()) {
            RADIO_DBG(printf("rb-probe TLS: refused HTTPS after AmiSSL poison host=%s port=%d\n", host, port);)
            rb_probe_transport_close(transport);
            return RB_STREAM_PROBE_ERR_TLS_POISONED;
        }
        ssl_connect_rc = 0;
        ssl_error = 0;
        ssl_lib_error = 0;
        ssl_error_buf[0] = '\0';
        sni_set = 0;
        if (rb_probe_ensure_amissl() != 0) {
            rb_probe_transport_close(transport);
            return RB_STREAM_PROBE_ERR_CONNECT;
        }
        transport->ctx = SSL_CTX_new(SSLv23_client_method());
        if (!transport->ctx) {
            rb_probe_transport_close(transport);
            return RB_STREAM_PROBE_ERR_CONNECT;
        }
        /* No CA bundle on classic AmigaOS; skip cert verification for streams. */
        SSL_CTX_set_verify(transport->ctx, SSL_VERIFY_NONE, NULL);
        transport->ssl = SSL_new(transport->ctx);
        if (!transport->ssl) {
            SSL_CTX_free(transport->ctx); transport->ctx = NULL;
            rb_probe_transport_close(transport);
            return RB_STREAM_PROBE_ERR_CONNECT;
        }
        SSL_set_fd(transport->ssl, (int)transport->sock);
#ifdef SSL_CTRL_SET_TLSEXT_HOSTNAME
        sni_set = (SSL_set_tlsext_host_name(transport->ssl, host) == 1);
#else
        sni_set = -1;
#endif
        RADIO_DBG(printf("rb-probe TLS: host=%s port=%d sni=%s verify=disabled method=SSLv23_client_method\n",
               host, port, sni_set > 0 ? host : (sni_set == 0 ? "not-set" : "unavailable"));)
        /* SSL_connect() can legitimately need several calls (WANT_READ/
         * WANT_WRITE) even on a nominally blocking socket -- retry with the
         * same budget radio_stream.c's radio_ssl_do_handshake() uses instead
         * of treating the first WANT_READ/WRITE as a hard failure. */
        /* Clean error queue first: a stale entry from an earlier failure
         * must not be misread as this connection's fatal error below. */
        ERR_clear_error();
        for (tries = 0; tries < 150; tries++) {
            ssl_connect_rc = SSL_connect(transport->ssl);
            if (ssl_connect_rc == 1) break;
            ssl_error = SSL_get_error(transport->ssl, ssl_connect_rc);
            if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
                rb_probe_backoff_sleep();
                continue;
            }
            break;
        }
        if (ssl_connect_rc != 1) {
            ssl_lib_error = ERR_get_error();
            if (ssl_lib_error != 0)
                ERR_error_string_n(ssl_lib_error, ssl_error_buf, sizeof(ssl_error_buf));
            RADIO_DBG(printf("rb-probe TLS: SSL_connect rc=%d SSL_get_error=%d error=\"%s\" verify=disabled method=SSLv23_client_method\n",
                   ssl_connect_rc, ssl_error, ssl_error_buf[0] ? ssl_error_buf : "none");)
            /* A handshake failure is a normal per-server outcome (protocol/
             * cipher mismatch, TLS-version-only server, bad record) -- e.g.
             * "0A000139 record layer failure" from a station AmiSSL simply
             * cannot talk to. It neither poisons AmiSSL nor counts toward
             * the fault fuse: probing an incompatible station must never
             * disable HTTPS. Free the objects normally and drain the error
             * queue so the failure cannot leak into a later session. */
            ERR_clear_error();
            if (Radio_IsTlsPoisoned()) {
                RADIO_DBG(printf("radio-cleanup: probe handshake-fail SSL_free/SSL_CTX_free skipped (poisoned) session=%lu ssl=%p ctx=%p\n", transport->session_id, (void *)transport->ssl, (void *)transport->ctx);)
                transport->ssl = NULL;
                transport->ctx = NULL;
            } else {
                rb_probe_debug_mem_report(transport->session_id, "before handshake-fail SSL_free");
                SSL_free(transport->ssl); transport->ssl = NULL;
                rb_probe_debug_mem_report(transport->session_id, "after handshake-fail SSL_free");
                SSL_CTX_free(transport->ctx); transport->ctx = NULL;
                rb_probe_debug_mem_report(transport->session_id, "after handshake-fail SSL_CTX_free");
            }
            rb_probe_transport_close(transport);
            return RB_STREAM_PROBE_ERR_TLS_HANDSHAKE;
        }
        transport->sslHandshakeDone = 1;
        transport->isSSL = 1;
    }
#else
    (void)use_ssl;
#endif
    return RB_STREAM_PROBE_OK;
}

static void rb_probe_transport_close_mode(RbProbeTransport *transport, RbProbeCloseMode mode, int http_status)
{
    if (!transport) return;
    RADIO_DBG(printf("radio-cleanup: probe close mode=%s session=%lu http_status=%d sslHandshakeDone=%d ssl=%p ctx=%p fd=%ld open_socket_count_before=%ld\n",
        rb_probe_close_mode_name(mode), transport->session_id, http_status,
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
        transport->sslHandshakeDone, (void *)transport->ssl, (void *)transport->ctx,
#else
        0, (void *)0, (void *)0,
#endif
        (long)transport->sock, rb_probe_open_socket_count);)
    if (transport->sock != RB_PROBE_INVALID_SOCKET) {
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
        int shutdown_called = 0;
        if (mode == RB_PROBE_CLOSE_GRACEFUL && transport->ssl && transport->sslHandshakeDone &&
            !transport->sslStatePoisoned && !Radio_IsTlsPoisoned()) {
            SSL_shutdown(transport->ssl);
            shutdown_called = 1;
        }
        RADIO_DBG(printf("radio-cleanup: probe ssl_shutdown session=%lu mode=%s %s\n",
            transport->session_id, rb_probe_close_mode_name(mode), shutdown_called ? "called" : "skipped");)
        if (transport->ssl && (transport->sslStatePoisoned || Radio_IsTlsPoisoned())) {
            RADIO_DBG(printf("radio-cleanup: probe SSL_free skipped (poisoned) session=%lu ssl=%p leaking to avoid crashing on corrupted AmiSSL state\n", transport->session_id, (void *)transport->ssl);)
            Radio_MarkTlsPoisoned("probe SSL_free skipped for poisoned AmiSSL session");
            transport->ssl = NULL;
            transport->sslHandshakeDone = 0;
        } else if (transport->ssl) {
            rb_probe_debug_mem_report(transport->session_id, "before close SSL_free");
            SSL_free(transport->ssl);
            transport->ssl = NULL;
            transport->sslHandshakeDone = 0;
            rb_probe_debug_mem_report(transport->session_id, "after close SSL_free");
        }
        if (transport->ctx && (transport->sslStatePoisoned || Radio_IsTlsPoisoned())) {
            RADIO_DBG(printf("radio-cleanup: probe SSL_CTX_free skipped (poisoned) session=%lu ctx=%p leaking to avoid crashing on corrupted AmiSSL state\n", transport->session_id, (void *)transport->ctx);)
            Radio_MarkTlsPoisoned("probe SSL_CTX_free skipped for poisoned AmiSSL session");
            transport->ctx = NULL;
        } else if (transport->ctx) {
            rb_probe_debug_mem_report(transport->session_id, "before close SSL_CTX_free");
            SSL_CTX_free(transport->ctx);
            transport->ctx = NULL;
            rb_probe_debug_mem_report(transport->session_id, "after close SSL_CTX_free");
        }
#endif
         {
            long closing_fd = (long)transport->sock;
            long before = rb_probe_open_socket_count;
            rb_probe_close_socket(transport->sock);
            transport->sock = RB_PROBE_INVALID_SOCKET;
            if (rb_probe_open_socket_count > 0) rb_probe_open_socket_count--;
            RADIO_DBG(printf("radio-socket: probe socket close session=%lu fd=%ld open_socket_count %ld->%ld probe_open_socket_count %ld->%ld\n", transport->session_id, closing_fd, before, rb_probe_open_socket_count, before, rb_probe_open_socket_count);)
        }
    }
    RADIO_DBG(printf("radio-cleanup: probe close mode=%s session=%lu complete open_socket_count_after=%ld\n",
        rb_probe_close_mode_name(mode), transport->session_id, rb_probe_open_socket_count);)
}

/* Safe default used by every connect/handshake/send/recv/redirect/HTTP-error
 * failure path in this file: never attempts SSL_shutdown(). */
static void rb_probe_transport_close(RbProbeTransport *transport)
{
    rb_probe_transport_close_mode(transport, RB_PROBE_CLOSE_ABORT, -1);
}

static int rb_probe_send_all(RbProbeTransport *transport, const char *buf, int len)
{
    int sent;
    int n;

    if (!transport || transport->sock == RB_PROBE_INVALID_SOCKET || !buf || len < 0)
        return RB_STREAM_PROBE_ERR_BAD_ARG;
    sent = 0;
    while (sent < len) {
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
        if (transport->isSSL && transport->ssl)
            n = (int)SSL_write(transport->ssl, buf + sent, len - sent);
        else
#endif
        n = (int)send(transport->sock, (char *)buf + sent, len - sent, 0);
        if (n <= 0) return RB_STREAM_PROBE_ERR_SEND;
        sent += n;
    }
    return RB_STREAM_PROBE_OK;
}

static int rb_probe_find_header_end(const unsigned char *buf, int len)
{
    int i;

    for (i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return i + 4;
    }
    return -1;
}

static int rb_probe_parse_int(const char *s)
{
    int value;

    value = 0;
    if (!s) return 0;
    while (*s == ' ' || *s == '\t') s++;
    while (*s >= '0' && *s <= '9') {
        value = value * 10 + (*s - '0');
        s++;
    }
    return value;
}

static void rb_probe_parse_headers(char *headers, int header_len, RbStreamInfo *info,
                                   char *location, int location_size)
{
    char *line;
    char *next;
    char *colon;
    char *value;
    int name_len;

    if (!headers || !info || header_len <= 0) return;
    headers[header_len] = '\0';
    line = headers;
    next = strstr(line, "\r\n");
    if (next) *next = '\0';
    if (strncmp(line, "HTTP/", 5) == 0) {
        sscanf(line, "%*s %d", &info->http_status);
    } else if (strncmp(line, "ICY ", 4) == 0) {
        sscanf(line, "ICY %d", &info->http_status);
    }
    line = next ? next + 2 : NULL;

    while (line && *line) {
        next = strstr(line, "\r\n");
        if (next) *next = '\0';
        colon = strchr(line, ':');
        if (colon) {
            name_len = (int)(colon - line);
            value = colon + 1;
            if (name_len == 12 && rb_probe_ascii_starts_nocase(line, "Content-Type"))
                rb_probe_copy_trim(info->content_type, (int)sizeof(info->content_type), value, (int)strlen(value));
            else if (name_len == 8 && rb_probe_ascii_starts_nocase(line, "icy-name"))
                rb_probe_copy_trim(info->icy_name, (int)sizeof(info->icy_name), value, (int)strlen(value));
            else if (name_len == 6 && rb_probe_ascii_starts_nocase(line, "icy-br"))
                info->icy_br = rb_probe_parse_int(value);
            else if (name_len == 11 && rb_probe_ascii_starts_nocase(line, "icy-metaint"))
                info->icy_metaint = rb_probe_parse_int(value);
            else if (name_len == 7 && rb_probe_ascii_starts_nocase(line, "icy-url"))
                rb_probe_copy_trim(info->icy_url, (int)sizeof(info->icy_url), value, (int)strlen(value));
            else if (name_len == 8 && rb_probe_ascii_starts_nocase(line, "Location") &&
                     location && location_size > 0)
                rb_probe_copy_trim(location, location_size, value, (int)strlen(value));
        }
        if (!next) break;
        line = next + 2;
    }
}

static int rb_probe_is_redirect_status(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

static int rb_probe_contains_nocase(const char *s, const char *needle)
{
    int needle_len;
    int i;

    if (!s || !needle) return 0;
    needle_len = (int)strlen(needle);
    if (needle_len <= 0) return 1;
    for (i = 0; s[i]; i++) {
        if (rb_probe_ascii_starts_nocase(s + i, needle)) return 1;
    }
    return 0;
}

static int rb_probe_url_has_mp3_hint(const RbProbeUrl *url)
{
    if (!url) return 0;
    return rb_probe_contains_nocase(url->path, "-mp3") ||
           rb_probe_contains_nocase(url->path, ".mp3") ||
           rb_probe_contains_nocase(url->path, "mp3");
}

static int rb_probe_url_has_aac_hint(const RbProbeUrl *url)
{
    if (!url) return 0;
    return rb_probe_contains_nocase(url->path, ".aac") ||
           rb_probe_contains_nocase(url->path, ".aacp") ||
           rb_probe_contains_nocase(url->path, "aac");
}

static int rb_probe_content_type_is_aac(const char *content_type)
{
    return rb_probe_contains_nocase(content_type, "audio/aac") ||
           rb_probe_contains_nocase(content_type, "audio/aacp") ||
           rb_probe_contains_nocase(content_type, "audio/x-aac") ||
           rb_probe_contains_nocase(content_type, "audio/mp4");
}

static RbStreamCodec rb_probe_detect_codec(const RbProbeUrl *url, const RbStreamInfo *info,
                                           const unsigned char *peek, int peek_len)
{
    if (peek && peek_len >= 3 && peek[0] == 'I' && peek[1] == 'D' && peek[2] == '3') {
        RADIO_DBG(printf("rb-probe codec: initial byte sniff=ID3 final=MP3\n");)
        return RB_STREAM_CODEC_MP3;
    }
    if (peek && peek_len >= 2 && peek[0] == 0xff && (peek[1] & 0xe0) == 0xe0 &&
        peek[1] != 0xf1 && peek[1] != 0xf9) {
        if ((info && rb_probe_content_type_is_aac(info->content_type)) || rb_probe_url_has_aac_hint(url)) {
            RADIO_DBG(printf("rb-probe codec conflict: AAC hint but first-byte sniff=MPEG/MP3; final=unsupported\n");)
            return RB_STREAM_CODEC_UNKNOWN;
        }
        RADIO_DBG(printf("rb-probe codec: initial byte sniff=MPEG frame sync final=MP3\n");)
        return RB_STREAM_CODEC_MP3;
    }
    if (peek && peek_len >= 2 && peek[0] == 0xff && (peek[1] == 0xf1 || peek[1] == 0xf9)) {
        RADIO_DBG(printf("rb-probe codec: initial byte sniff=ADTS final=AAC\n");)
        return RB_STREAM_CODEC_AAC;
    }
    if (peek && peek_len >= 4 && peek[0] == 'O' && peek[1] == 'g' && peek[2] == 'g' && peek[3] == 'S') {
        /* "OggS" is just the container magic -- Vorbis, Opus, Speex and
         * FLAC-in-Ogg all start a page with it. The only codec this app can
         * actually decode inside an Ogg container is Vorbis (via Tremor);
         * an Opus (or other) payload handed to ogg.decoder isn't a format
         * error Tremor is guaranteed to reject cleanly, so catch it here
         * instead of letting ov_open_callbacks() ever see it. Every Opus
         * stream's very first page carries an "OpusHead" identification
         * packet, which -- unlike Vorbis's packet-type-prefixed "vorbis"
         * string -- is safe to substring-search for regardless of the exact
         * page header length (segment table size varies). */
        int i;
        for (i = 0; i + 8 <= peek_len; i++) {
            if (peek[i] == 'O' && peek[i + 1] == 'p' && peek[i + 2] == 'u' && peek[i + 3] == 's' &&
                peek[i + 4] == 'H' && peek[i + 5] == 'e' && peek[i + 6] == 'a' && peek[i + 7] == 'd') {
                RADIO_DBG(printf("rb-probe codec: initial byte sniff=OggS+OpusHead final=unsupported (Opus-in-Ogg)\n");)
                return RB_STREAM_CODEC_UNKNOWN;
            }
        }
        RADIO_DBG(printf("rb-probe codec: initial byte sniff=OggS final=OGG\n");)
        return RB_STREAM_CODEC_OGG;
    }
    if (info && info->content_type[0]) {
        if (rb_probe_contains_nocase(info->content_type, "audio/mpeg") ||
            rb_probe_contains_nocase(info->content_type, "audio/mp3")) return RB_STREAM_CODEC_MP3;
        if (rb_probe_content_type_is_aac(info->content_type)) return RB_STREAM_CODEC_AAC;
        if (rb_probe_contains_nocase(info->content_type, "audio/ogg") ||
            rb_probe_contains_nocase(info->content_type, "application/ogg") ||
            rb_probe_contains_nocase(info->content_type, "audio/vorbis") ||
            rb_probe_contains_nocase(info->content_type, "audio/x-vorbis")) return RB_STREAM_CODEC_OGG;
    }
    if (rb_probe_url_has_aac_hint(url))
        return RB_STREAM_CODEC_AAC;
    if (url && (rb_probe_contains_nocase(url->path, ".ogg") || rb_probe_contains_nocase(url->path, ".oga")))
        return RB_STREAM_CODEC_OGG;
    if (rb_probe_url_has_mp3_hint(url))
        return RB_STREAM_CODEC_MP3;
    return RB_STREAM_CODEC_UNKNOWN;
}

static int rb_probe_is_hls(const RbProbeUrl *url, const RbStreamInfo *info)
{
    if (info && info->content_type[0] &&
        (rb_probe_contains_nocase(info->content_type, "application/vnd.apple.mpegurl") ||
         rb_probe_contains_nocase(info->content_type, "application/x-mpegurl")))
        return 1;
    if (url && rb_probe_contains_nocase(url->path, ".m3u8"))
        return 1;
    return 0;
}

int rb_probe_url_looks_hls(const char *url)
{
    RbProbeUrl parsed;

    if (!url) return 0;
    if (rb_probe_parse_url(url, &parsed) == RB_STREAM_PROBE_OK &&
        rb_probe_contains_nocase(parsed.path, ".m3u8"))
        return 1;
    return rb_probe_contains_nocase(url, ".m3u8");
}

const char *rb_probe_error_text(int rc)
{
    switch (rc) {
    case RB_STREAM_PROBE_ERR_BAD_ARG: return "Stream probe failed: bad argument";
    case RB_STREAM_PROBE_ERR_UNSUPPORTED_TLS: return "HTTPS not supported in this build";
    case RB_STREAM_PROBE_ERR_BAD_URL: return "Stream probe failed: bad or redirected URL";
    case RB_STREAM_PROBE_ERR_DNS: return "Stream probe failed: cannot resolve host";
    case RB_STREAM_PROBE_ERR_CONNECT: return "Stream probe failed: timeout while connecting";
    case RB_STREAM_PROBE_ERR_SEND: return "Stream probe failed: server closed connection while sending request";
    case RB_STREAM_PROBE_ERR_RECV: return "Stream probe failed: timeout while reading headers";
    case RB_STREAM_PROBE_ERR_HEADERS_TOO_BIG: return "Stream probe failed: response headers too large";
    case RB_STREAM_PROBE_ERR_REQUEST_TOO_BIG: return "Stream probe failed: request too large";
    case RB_STREAM_PROBE_ERR_TOO_MANY_REDIRECTS: return "Stream probe failed: too many redirects";
    case RB_STREAM_PROBE_ERR_URL_TOO_LONG: return "Stream probe failed: URL too long";
    case RB_STREAM_PROBE_ERR_HLS_UNSUPPORTED: return "HLS stream not supported";
    case RB_STREAM_PROBE_ERR_UNSUPPORTED_CONTENT_TYPE: return "Unsupported stream format";
    case RB_STREAM_PROBE_ERR_SERVER_CLOSED: return "Stream probe failed: server closed connection during probe";
    case RB_STREAM_PROBE_ERR_TLS_HANDSHAKE: return "TLS handshake failed";
    case RB_STREAM_PROBE_ERR_TLS_POISONED: return "HTTPS disabled after fatal TLS/memory faults; reboot before using HTTPS.";
    case RB_STREAM_PROBE_ERR_HTTP_STATUS: return "Stream unavailable (server returned an error status)";
    default: return "Stream probe failed";
    }
}

static int rb_probe_stream_url_impl(const char *url, RbStreamInfo *info,
                        unsigned char *peek_buf, int peek_buf_size, int *peek_len);

/* Public entry point.  Wraps the implementation so the probe's per-task AmiSSL
 * and socket state is always torn down, no matter which exit path the impl took.
 * The many error returns inside the impl (server closed mid-handshake, recv
 * error, non-2xx status, redirect failures, HLS, ...) used to skip cleanup,
 * leaving the main/GUI task's AmiSSL session half-open after a flaky stream.
 * The next probe then short-circuited on that stale session and could wedge the
 * whole machine ("play stuck" right after a probe that returned an error). */
int rb_probe_stream_url(const char *url, RbStreamInfo *info,
                        unsigned char *peek_buf, int peek_buf_size, int *peek_len)
{
    int rc = rb_probe_stream_url_impl(url, info, peek_buf, peek_buf_size, peek_len);
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    rb_probe_cleanup_amissl();
#elif defined(AMIGA_M68K) && !defined(RB_STREAM_PROBE_EXTERNAL_SOCKETBASE)
    rb_probe_release_idle_socketbase();
#endif
    return rc;
}

static int rb_probe_stream_url_impl(const char *url, RbStreamInfo *info,
                        unsigned char *peek_buf, int peek_buf_size, int *peek_len)
{
    RbProbeUrl parsed;
    RbProbeTransport transport;
    char request[RB_PROBE_MAX_REQUEST];
    unsigned char header_buf[RB_PROBE_HEADER_BUF + 1];
    char parse_buf[RB_PROBE_HEADER_BUF + 1];
    char current_url[RB_PROBE_MAX_URL];
    char next_url[RB_PROBE_MAX_URL];
    char location[RB_PROBE_MAX_URL];
    int rc;
    int request_len;
    int total;
    int header_end;
    int done;
    int redirects;

    if (!url || !info || !peek_len || peek_buf_size < 0 || (peek_buf_size > 0 && !peek_buf))
        return RB_STREAM_PROBE_ERR_BAD_ARG;
    if (!strncmp(url, "https://", 8) && Radio_IsTlsPoisoned()) {
        RADIO_DBG(printf("rb-probe: refused HTTPS probe after AmiSSL poison url=\"%s\"\n", url);)
        return RB_STREAM_PROBE_ERR_TLS_POISONED;
    }
    rb_probe_info_init(info);
    *peek_len = 0;
    if (rb_probe_url_looks_hls(url)) return RB_STREAM_PROBE_ERR_HLS_UNSUPPORTED;
    rc = rb_probe_copy_string(current_url, (int)sizeof(current_url), url);
    if (rc < 0) return rc;
    redirects = 0;
    for (;;) {
        *peek_len = 0;
        rb_probe_info_init(info);
        info->redirect_count = redirects;
        rc = rb_probe_set_final_url(info, current_url);
        if (rc < 0) return rc;
        location[0] = '\0';

        rc = rb_probe_parse_url(current_url, &parsed);
        if (rc < 0) return rc;
        rc = rb_probe_build_request(request, (int)sizeof(request), &parsed);
        if (rc < 0) return rc;
        request_len = (int)strlen(request);
        rc = rb_probe_transport_open(&transport, parsed.host, parsed.port, parsed.isSSL, &info->host_addr_be);
        if (rc == RB_STREAM_PROBE_OK)
            info->have_host_addr = 1;
        if (rc < 0) return rc;
        rc = rb_probe_send_all(&transport, request, request_len);
        if (rc < 0) {
            rb_probe_transport_close(&transport);
            return rc;
        }
        total = 0;
        header_end = -1;
        done = 0;
        while (!done) {
            int want;
            int n;
            int body_avail;
            int copy;

            want = RB_PROBE_HEADER_BUF - total;
            if (want > RB_PROBE_READ_CHUNK) want = RB_PROBE_READ_CHUNK;
            if (want <= 0) {
                rb_probe_transport_close(&transport);
                return RB_STREAM_PROBE_ERR_HEADERS_TOO_BIG;
            }
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
            if (transport.isSSL && transport.ssl) {
                int capacity = RB_PROBE_HEADER_BUF - total;
                if (want > capacity) want = capacity;
                n = rb_probe_ssl_read_retrying(&transport, (char *)header_buf + total, want, capacity, total);
            } else
#endif
            n = (int)recv(transport.sock, (char *)header_buf + total, want, 0);
            if (n < 0) {
                rb_probe_transport_close(&transport);
                return RB_STREAM_PROBE_ERR_RECV;
            }
            if (n == 0) {
                rb_probe_transport_close(&transport);
                return RB_STREAM_PROBE_ERR_SERVER_CLOSED;
            }
            total += n;
            if (header_end < 0) header_end = rb_probe_find_header_end(header_buf, total);
            if (header_end >= 0) {
                body_avail = total - header_end;
                copy = body_avail;
                if (copy > peek_buf_size - *peek_len) copy = peek_buf_size - *peek_len;
                if (copy > 0) {
                    memcpy(peek_buf + *peek_len, header_buf + header_end, (size_t)copy);
                    *peek_len += copy;
                }
                done = (*peek_len >= peek_buf_size);
                break;
            }
        }
        if (header_end < 0) {
            rb_probe_transport_close(&transport);
            return RB_STREAM_PROBE_ERR_HEADERS_TOO_BIG;
        }
        memcpy(parse_buf, header_buf, (size_t)header_end);
        rb_probe_parse_headers(parse_buf, header_end, info, location, (int)sizeof(location));
        if (rb_probe_is_hls(&parsed, info)) {
            rb_probe_transport_close(&transport);
            return RB_STREAM_PROBE_ERR_HLS_UNSUPPORTED;
        }
        if (!rb_probe_is_redirect_status(info->http_status)) break;
        rb_probe_transport_close_mode(&transport, RB_PROBE_CLOSE_ABORT, info->http_status);
        *peek_len = 0;
        if (!location[0]) return RB_STREAM_PROBE_ERR_BAD_URL;
        if (redirects >= RB_PROBE_MAX_REDIRECTS) return RB_STREAM_PROBE_ERR_TOO_MANY_REDIRECTS;
        rc = rb_probe_resolve_location(&parsed, location, next_url, (int)sizeof(next_url));
        if (rc < 0) return rc;
        redirects++;
        rc = rb_probe_copy_string(current_url, (int)sizeof(current_url), next_url);
        if (rc < 0) return rc;
    }
    /* Only a 2xx response is playable.  A dead/auth-protected stream answers
     * with 401/403/404/5xx (and usually an empty body), so without this check
     * the probe would guess the codec from the URL filename and report success,
     * launching a doomed playback child that buffers nothing and then errors or
     * stalls.  Reject it here so the GUI shows a clear "stream unavailable"
     * message and never starts playback. */
    if (info->http_status < 200 || info->http_status > 299) {
        rb_probe_transport_close_mode(&transport, RB_PROBE_CLOSE_ABORT, info->http_status);
        return RB_STREAM_PROBE_ERR_HTTP_STATUS;
    }
    while (*peek_len < peek_buf_size) {
        int want2;
        int n2;

        want2 = peek_buf_size - *peek_len;
        if (want2 > RB_PROBE_READ_CHUNK) want2 = RB_PROBE_READ_CHUNK;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
        if (transport.isSSL && transport.ssl) {
            int capacity2 = peek_buf_size - *peek_len;
            if (want2 > capacity2) want2 = capacity2;
            n2 = rb_probe_ssl_read_retrying(&transport, (char *)peek_buf + *peek_len, want2, capacity2, *peek_len);
        } else
#endif
        n2 = (int)recv(transport.sock, (char *)peek_buf + *peek_len, want2, 0);
        if (n2 < 0) {
            rb_probe_transport_close(&transport);
            return RB_STREAM_PROBE_ERR_RECV;
        }
        if (n2 == 0) break;
        *peek_len += n2;
    }
    /* Always abort here, never SSL_shutdown(): this probe runs before every
     * single station switch, so it is the most frequently exercised close in
     * the app, and calling SSL_shutdown() from any of this app's teardown
     * paths (this one, the favicon fetch below, and Radio_RequestStop()) is
     * what has been producing AN_BadFreeAddr (0x0100000F) recoverable
     * alerts.  Skipping the clean TLS close_notify is harmless. */
    rb_probe_transport_close_mode(&transport, RB_PROBE_CLOSE_ABORT, info->http_status);
    info->redirect_count = redirects;
    rc = rb_probe_set_final_url(info, current_url);
    if (rc < 0) {
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
        rb_probe_cleanup_amissl();
#endif
        return rc;
    }
    RADIO_DBG(printf("rb-probe codec: final URL=%s content-type=%s URL codec hint=%s initial-bytes=%d\n",
           current_url, info->content_type, rb_probe_url_has_aac_hint(&parsed) ? "AAC" : (rb_probe_url_has_mp3_hint(&parsed) ? "MP3" : "none"), *peek_len);)
    info->codec = rb_probe_detect_codec(&parsed, info, peek_buf, *peek_len);
    RADIO_DBG(printf("rb-probe codec: final selected codec=%s\n",
           info->codec == RB_STREAM_CODEC_MP3 ? "MP3" : (info->codec == RB_STREAM_CODEC_AAC ? "AAC" : (info->codec == RB_STREAM_CODEC_OGG ? "OGG" : "unsupported")));)
    if (rb_probe_is_hls(&parsed, info)) {
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
        rb_probe_cleanup_amissl();
#endif
        return RB_STREAM_PROBE_ERR_HLS_UNSUPPORTED;
    }
    if (info->content_type[0] && info->codec == RB_STREAM_CODEC_UNKNOWN) {
        RADIO_DBG(printf("rb-probe cleanup: unsupported cleanup start final_url=%s content_type=%s\n", current_url, info->content_type);)
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
        rb_probe_cleanup_amissl();
#endif
        RADIO_DBG(printf("rb-probe cleanup: unsupported cleanup end final_state=ERROR codec=unsupported\n");)
        return RB_STREAM_PROBE_ERR_UNSUPPORTED_CONTENT_TYPE;
    }
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    rb_probe_cleanup_amissl();
#endif
    return RB_STREAM_PROBE_OK;
}

static int rb_probe_fetch_binary_impl(const char *url, unsigned char *out_buf, int out_buf_size,
                        int *out_len, char *out_content_type, int out_content_type_size)
{
    /* Static, not stack-local: unlike rb_probe_stream_url_impl() (called
     * from the playback child process, which gets a generous stack of its
     * own), this path runs synchronously on the main GUI task from deep
     * inside menu/event handling.  These buffers alone are >12KB; stacked
     * on top of whatever the GUI call chain already used, that overran the
     * GUI task's default stack and corrupted adjacent memory (Guru
     * 81000005).  Not reentrant, but this function is never called
     * concurrently with itself. */
    static RbProbeUrl parsed;
    static RbProbeTransport transport;
    static RbStreamInfo info;
    static char request[RB_PROBE_MAX_REQUEST];
    static unsigned char header_buf[RB_PROBE_HEADER_BUF + 1];
    static char parse_buf[RB_PROBE_HEADER_BUF + 1];
    static char current_url[RB_PROBE_MAX_URL];
    static char next_url[RB_PROBE_MAX_URL];
    static char location[RB_PROBE_MAX_URL];
    int rc;
    int request_len;
    int total;
    int header_end;
    int redirects;

    if (!url || !out_buf || out_buf_size <= 0 || !out_len) return RB_STREAM_PROBE_ERR_BAD_ARG;
    if (!strncmp(url, "https://", 8) && Radio_IsTlsPoisoned()) {
        RADIO_DBG(printf("rb-probe: refused HTTPS fetch after AmiSSL poison url=\"%s\"\n", url);)
        return RB_STREAM_PROBE_ERR_TLS_POISONED;
    }
    *out_len = 0;
    if (out_content_type && out_content_type_size > 0) out_content_type[0] = '\0';

    rc = rb_probe_copy_string(current_url, (int)sizeof(current_url), url);
    if (rc < 0) return rc;
    redirects = 0;
    for (;;) {
        rb_probe_info_init(&info);
        location[0] = '\0';
        *out_len = 0;

        rc = rb_probe_parse_url(current_url, &parsed);
        if (rc < 0) return rc;
        rc = rb_probe_build_request(request, (int)sizeof(request), &parsed);
        if (rc < 0) return rc;
        request_len = (int)strlen(request);
        rc = rb_probe_transport_open(&transport, parsed.host, parsed.port, parsed.isSSL, NULL);
        if (rc < 0) return rc;
        rc = rb_probe_send_all(&transport, request, request_len);
        if (rc < 0) {
            rb_probe_transport_close(&transport);
            return rc;
        }

        total = 0;
        header_end = -1;
        for (;;) {
            int want;
            int n;
            int body_avail;
            int copy;

            want = RB_PROBE_HEADER_BUF - total;
            if (want > RB_PROBE_READ_CHUNK) want = RB_PROBE_READ_CHUNK;
            if (want <= 0) {
                rb_probe_transport_close(&transport);
                return RB_STREAM_PROBE_ERR_HEADERS_TOO_BIG;
            }
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
            if (transport.isSSL && transport.ssl)
                n = rb_probe_ssl_read_retrying(&transport, (char *)header_buf + total, want, RB_PROBE_HEADER_BUF - total, total);
            else
#endif
            n = (int)recv(transport.sock, (char *)header_buf + total, want, 0);
            if (n < 0) {
                rb_probe_transport_close(&transport);
                return RB_STREAM_PROBE_ERR_RECV;
            }
            if (n == 0) {
                rb_probe_transport_close(&transport);
                return RB_STREAM_PROBE_ERR_SERVER_CLOSED;
            }
            total += n;
            if (header_end < 0) header_end = rb_probe_find_header_end(header_buf, total);
            if (header_end >= 0) {
                body_avail = total - header_end;
                copy = body_avail;
                if (copy > out_buf_size - *out_len) copy = out_buf_size - *out_len;
                if (copy > 0) {
                    memcpy(out_buf + *out_len, header_buf + header_end, (size_t)copy);
                    *out_len += copy;
                }
                break;
            }
        }
        if (header_end < 0) {
            rb_probe_transport_close(&transport);
            return RB_STREAM_PROBE_ERR_HEADERS_TOO_BIG;
        }
        memcpy(parse_buf, header_buf, (size_t)header_end);
        rb_probe_parse_headers(parse_buf, header_end, &info, location, (int)sizeof(location));
        if (!rb_probe_is_redirect_status(info.http_status)) break;
        rb_probe_transport_close_mode(&transport, RB_PROBE_CLOSE_ABORT, info.http_status);
        *out_len = 0;
        if (!location[0]) return RB_STREAM_PROBE_ERR_BAD_URL;
        if (redirects >= RB_PROBE_MAX_REDIRECTS) return RB_STREAM_PROBE_ERR_TOO_MANY_REDIRECTS;
        rc = rb_probe_resolve_location(&parsed, location, next_url, (int)sizeof(next_url));
        if (rc < 0) return rc;
        redirects++;
        rc = rb_probe_copy_string(current_url, (int)sizeof(current_url), next_url);
        if (rc < 0) return rc;
    }

    /* Drain the remaining body, bounded to out_buf_size -- the caller (e.g.
     * the radio favicon loader) sizes out_buf_size to its own download cap. */
    while (*out_len < out_buf_size) {
        int want;
        int n;

        want = out_buf_size - *out_len;
        if (want > RB_PROBE_READ_CHUNK) want = RB_PROBE_READ_CHUNK;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
        if (transport.isSSL && transport.ssl)
            n = rb_probe_ssl_read_retrying(&transport, (char *)out_buf + *out_len, want, out_buf_size - *out_len, *out_len);
        else
#endif
        n = (int)recv(transport.sock, (char *)out_buf + *out_len, want, 0);
        if (n < 0) {
            rb_probe_transport_close(&transport);
            return RB_STREAM_PROBE_ERR_RECV;
        }
        if (n == 0) break;
        *out_len += n;
    }
    /* Always abort (see the matching comment in rb_probe_stream_url_impl()):
     * this runs on every favicon fetch, so a graceful SSL_shutdown() here is
     * just as frequent a source of AN_BadFreeAddr as the station probe. */
    rb_probe_transport_close_mode(&transport, RB_PROBE_CLOSE_ABORT, info.http_status);

    if (info.http_status < 200 || info.http_status > 299)
        return RB_STREAM_PROBE_ERR_HTTP_STATUS;
    if (out_content_type && out_content_type_size > 0) {
        int n = (int)strlen(info.content_type);
        if (n > out_content_type_size - 1) n = out_content_type_size - 1;
        memcpy(out_content_type, info.content_type, (size_t)n);
        out_content_type[n] = '\0';
    }
    return RB_STREAM_PROBE_OK;
}

/* Public entry point; see rb_probe_stream_url() above for why the per-task
 * AmiSSL/socket cleanup always runs regardless of which path returned. */
int rb_probe_fetch_binary(const char *url, unsigned char *out_buf, int out_buf_size,
                        int *out_len, char *out_content_type, int out_content_type_size)
{
    int rc = rb_probe_fetch_binary_impl(url, out_buf, out_buf_size, out_len,
                                         out_content_type, out_content_type_size);
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    rb_probe_cleanup_amissl();
#elif defined(AMIGA_M68K) && !defined(RB_STREAM_PROBE_EXTERNAL_SOCKETBASE)
    rb_probe_release_idle_socketbase();
#endif
    return rc;
}

#ifdef RB_STREAM_PROBE_TEST
static const char *rb_probe_codec_name(RbStreamCodec codec)
{
    switch (codec) {
    case RB_STREAM_CODEC_MP3: return "MP3";
    case RB_STREAM_CODEC_AAC: return "AAC";
    case RB_STREAM_CODEC_OGG: return "OGG";
    default: return "unknown";
    }
}

static int rb_probe_selftest(void)
{
    RbProbeUrl url;
    RbStreamInfo info;
    unsigned char id3[] = { 'I', 'D', '3', 4, 0, 0 };
    unsigned char mpeg[] = { 0xff, 0xfb, 0x90, 0x64 };

    rb_probe_info_init(&info);
    if (rb_probe_parse_url("http://ice1.somafm.com/groovesalad-128-mp3", &url) != RB_STREAM_PROBE_OK) return 1;
    if (rb_probe_detect_codec(&url, &info, NULL, 0) != RB_STREAM_CODEC_MP3) return 2;
    if (rb_probe_parse_url("http://ice1.somafm.com/groovesalad-64-mp3", &url) != RB_STREAM_PROBE_OK) return 3;
    if (rb_probe_detect_codec(&url, &info, NULL, 0) != RB_STREAM_CODEC_MP3) return 4;
    if (rb_probe_detect_codec(&url, &info, id3, (int)sizeof(id3)) != RB_STREAM_CODEC_MP3) return 5;
    if (rb_probe_detect_codec(&url, &info, mpeg, (int)sizeof(mpeg)) != RB_STREAM_CODEC_MP3) return 6;
    rb_probe_copy_trim(info.content_type, (int)sizeof(info.content_type), "audio/aacp", 10);
    if (rb_probe_detect_codec(&url, &info, mpeg, (int)sizeof(mpeg)) != RB_STREAM_CODEC_UNKNOWN) return 7;
    rb_probe_info_init(&info);
    rb_probe_copy_trim(info.content_type, (int)sizeof(info.content_type), "audio/mp4", 9);
    if (rb_probe_detect_codec(&url, &info, NULL, 0) != RB_STREAM_CODEC_AAC) return 8;
    if (!rb_probe_url_looks_hls("http://example.com/live.m3u8")) return 9;
    RADIO_DBG(printf("rb-probe selftest: ok\n");)
    return 0;
}

int main(int argc, char **argv)
{
    RbStreamInfo info;
    unsigned char peek[512];
    int peek_len;
    int rc;

    if (argc >= 2 && strcmp(argv[1], "--selftest") == 0)
        return rb_probe_selftest();
    if (argc < 2) {
        fprintf(stderr, "usage: %s URL | --selftest\n", argv[0]);
        return 2;
    }
    rc = rb_probe_stream_url(argv[1], &info, peek, (int)sizeof(peek), &peek_len);
    if (rc < 0) {
        printf("probe error: %d\n", rc);
        return 1;
    }
    printf("status: %d\n", info.http_status);
    printf("redirects followed: %d\n", info.redirect_count);
    printf("final url: %s\n", info.final_url);
    RADIO_DBG(printf("content type: %s\n", info.content_type);)
    printf("icy name: %s\n", info.icy_name);
    printf("icy bitrate: %d\n", info.icy_br);
    printf("icy metaint: %d\n", info.icy_metaint);
    RADIO_DBG(printf("detected codec: %s\n", rb_probe_codec_name(info.codec));)
    RADIO_DBG(printf("peek bytes: %d\n", peek_len);)
    return 0;
}
#endif
