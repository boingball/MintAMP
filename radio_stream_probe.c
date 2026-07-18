/*
 * Small fixed-buffer HTTP/ICY stream probe.
 *
 * Test harness build:
 *   gcc -std=gnu89 -Wall -Wextra -DRB_STREAM_PROBE_TEST radio_stream_probe.c -o /tmp/rb_stream_probe_test
 */

#include "radio_stream_probe.h"
#include "radio_debug.h"
#include "radio_stream.h"
#include "radio_runtime_flags.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "miniamp_memguard.h"

#if defined(AMIGA_M68K)
#include <stdarg.h>
#include <exec/semaphores.h>
#include <proto/exec.h>
/* Same cross-task shared-stdout race as radio_stream.c (see radio_debug.h's
 * RADIO_DBG comment for the field evidence): this file's probe/favicon
 * fetches run on the net worker task (or the GUI task's lazy fallback) and
 * have many always-on printf() calls that were unlocked. Redirect every
 * printf() in this file through the same shared radio_console_lock. */
extern struct SignalSemaphore radio_console_lock;
static int RadioStreamProbeLockedPrintf(const char *fmt, ...)
{
    int r;
    va_list ap;
    ObtainSemaphore(&radio_console_lock);
    va_start(ap, fmt);
    r = vprintf(fmt, ap);
    va_end(ap);
    ReleaseSemaphore(&radio_console_lock);
    return r;
}
#ifdef printf
#undef printf
#endif
#define printf RadioStreamProbeLockedPrintf
#endif

#if !defined(AMIGA_M68K)
#include <unistd.h>
#endif

static void rb_probe_debug_mem_report(unsigned long session_id, const char *where)
{
    RADIO_DBG(printf("rb-probe-mem: session=%lu %s transport-owned-by-radio_stream.c\n",
        session_id, where ? where : ""));
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
/* ~6s at ~40ms/poll -- same connect budget as radio_stream.c's
 * radio_wait_connected(). */
#define RB_PROBE_CONNECT_TRIES 150
/* ~10s of no data before giving up on a stalled send/transport.  This whole probe
 * runs synchronously on the main GUI task (see rb_probe_fetch_binary_impl()'s
 * comment), so on a blocking socket a server that accepts the TCP connection
 * and then never answers -- or answers the headers and then goes silent --
 * hangs this call forever with no non-blocking mode of its own to fall back
 * on, freezing the whole GUI (Stop included, since it never gets back to the
 * event loop to see the click) until the machine is rebooted. */
#define RB_PROBE_IO_STALL_TRIES 250

static unsigned long rb_probe_next_session_id = 1;
static long rb_probe_open_socket_count = 0;
static long rb_probe_active_ssl_count = 0;
static long rb_probe_zero_return_count = 0;
static long rb_probe_zero_return_ssl_free_count = 0;
static long rb_probe_ssl_free_skipped_poison_count = 0;
static int rb_probe_artwork_disabled_for_run = 0;

#if defined(AMIGA_M68K)
static void rb_probe_backoff_sleep(void)
{
    Delay(2); /* ~40ms (2 ticks @ 50Hz), same budget as radio_stream.c's handshake poll */
}
#else
static void rb_probe_backoff_sleep(void)
{
    usleep(40000);
}
#endif

/* Transport close mode: GRACEFUL marks a healthy application close eligible for
 * one best-effort SSL_shutdown(); ABORT is for ordinary failed HTTP/socket
 * paths.  Actual fatal TLS/memory poison is classified from the transport and
 * always gets raw-socket-only quarantine. */
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
    RadioNetTransport *net;
    int isSSL;
    unsigned long session_id;
    char host[RB_PROBE_MAX_HOST];
    char url[RB_PROBE_MAX_URL];
    const char *category;
} RbProbeTransport;

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

static char *rb_probe_dup_string(const char *src)
{
    char *copy;
    size_t n;
    if (!src) src = "";
    n = strlen(src) + 1;
    copy = (char *)malloc(n);
    if (copy) memcpy(copy, src, n);
    return copy;
}

#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
enum {
    RB_PROBE_REQ_PENDING = 0,
    RB_PROBE_REQ_COMPLETED = 1,
    RB_PROBE_REQ_ABANDONED = 2
};

typedef struct RbProbeRequestSync {
    int state;
    struct SignalSemaphore lock;
} RbProbeRequestSync;

static void rb_probe_request_sync_init(RbProbeRequestSync *sync)
{
    if (!sync) return;
    sync->state = RB_PROBE_REQ_PENDING;
    InitSemaphore(&sync->lock);
}

static void rb_probe_request_lock(RbProbeRequestSync *sync)
{
    if (sync) ObtainSemaphore(&sync->lock);
}

static void rb_probe_request_unlock(RbProbeRequestSync *sync)
{
    if (sync) ReleaseSemaphore(&sync->lock);
}
#endif

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

void rb_probe_shutdown_tls_context(void) { }

static void rb_probe_transport_close_mode(RbProbeTransport *transport, RbProbeCloseMode mode, int http_status);
static void rb_probe_transport_close(RbProbeTransport *transport);

static int rb_probe_transport(RbProbeTransport *transport, void *buf, int want)
{
    int n;
    (void)want;
    if (!transport || !transport->net || !buf || want <= 0) return -1;
    n = RadioNet_Read(transport->net, buf, want);
    if (n == 0) rb_probe_zero_return_count++;
    return n;
}

static int rb_probe_transport_open_ex(RbProbeTransport *transport, const char *url, const char *host, int port, int use_ssl, const char *category, unsigned long *host_addr_be, int private_ctx)
{
    (void)private_ctx;
    if (!transport || !host) return RB_STREAM_PROBE_ERR_BAD_ARG;
    memset(transport, 0, sizeof(*transport));
    transport->isSSL = use_ssl;
    transport->session_id = rb_probe_next_session_id++;
    rb_probe_copy_trim(transport->host, (int)sizeof(transport->host), host, (int)strlen(host));
    rb_probe_copy_string(transport->url, (int)sizeof(transport->url), url ? url : "");
    transport->category = category ? category : "probe";
    Radio_SetTlsFaultContext(transport->session_id, transport->url);
    transport->net = RadioNet_Open(url, host, port, use_ssl, transport->category, transport->session_id);
    if (!transport->net) return use_ssl && Radio_IsTlsPoisoned() ? RB_STREAM_PROBE_ERR_TLS_POISONED : RB_STREAM_PROBE_ERR_CONNECT;
    rb_probe_open_socket_count++;
    if (use_ssl) rb_probe_active_ssl_count++;
    if (host_addr_be) {
        *host_addr_be = RadioNet_HostAddr(transport->net);
        if (*host_addr_be) {
            char addr_text[16];
            rb_probe_format_ipv4_be(*host_addr_be, addr_text, (int)sizeof(addr_text));
            RADIO_DBG(printf("rb-probe DNS: resolved %s -> %s\n", host, addr_text);)
        }
    }
    return RB_STREAM_PROBE_OK;
}

static int rb_probe_transport_open(RbProbeTransport *transport, const char *url, const char *host, int port, int use_ssl, unsigned long *host_addr_be)
{
    return rb_probe_transport_open_ex(transport, url, host, port, use_ssl, "probe", host_addr_be, 0);
}

static int rb_probe_transport_open_artwork(RbProbeTransport *transport, const char *url, const char *host, int port, int use_ssl)
{
    return rb_probe_transport_open_ex(transport, url, host, port, use_ssl, "artwork", NULL, 1);
}

static void rb_probe_transport_close_mode(RbProbeTransport *transport, RbProbeCloseMode mode, int http_status)
{
    (void)http_status;
    if (!transport || !transport->net) return;
    RadioNet_Close(transport->net, mode == RB_PROBE_CLOSE_GRACEFUL);
    transport->net = NULL;
    if (rb_probe_open_socket_count > 0) rb_probe_open_socket_count--;
    if (transport->isSSL && rb_probe_active_ssl_count > 0) rb_probe_active_ssl_count--;
}

static void rb_probe_transport_close(RbProbeTransport *transport)
{
    rb_probe_transport_close_mode(transport, RB_PROBE_CLOSE_ABORT, -1);
}

static int rb_probe_send_all(RbProbeTransport *transport, const char *buf, int len)
{
    if (!transport || !transport->net || !buf || len < 0) return RB_STREAM_PROBE_ERR_BAD_ARG;
    return RadioNet_Write(transport->net, buf, len) == 0 ? RB_STREAM_PROBE_OK : RB_STREAM_PROBE_ERR_SEND;
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
    /* "OggS" is just the container magic -- Vorbis, Opus, Speex and
     * FLAC-in-Ogg all start a page with it. The only codec this app can
     * actually decode inside an Ogg container is Vorbis (via Tremor);
     * an Opus (or other) payload handed to ogg.decoder isn't a format
     * error Tremor is guaranteed to reject cleanly, so this must be
     * checked before EVERY path below that can return RB_STREAM_CODEC_OGG
     * -- including the Content-Type and URL-extension fallbacks, since a
     * real Icecast Opus-in-Ogg mount reports the exact same generic
     * "audio/ogg"/"application/ogg" Content-Type a Vorbis mount does, and
     * some servers' first response chunk is too short for the byte-sniff
     * below to see the "OggS" magic at all. Every Opus stream's very first
     * page carries an "OpusHead" identification packet, which -- unlike
     * Vorbis's packet-type-prefixed "vorbis" string -- is safe to
     * substring-search for regardless of the exact page header length
     * (segment table size varies). */
    if (peek) {
        int i;
        for (i = 0; i + 8 <= peek_len; i++) {
            if (peek[i] == 'O' && peek[i + 1] == 'p' && peek[i + 2] == 'u' && peek[i + 3] == 's' &&
                peek[i + 4] == 'H' && peek[i + 5] == 'e' && peek[i + 6] == 'a' && peek[i + 7] == 'd') {
                RADIO_DBG(printf("rb-probe codec: peek buffer contains OpusHead final=unsupported (Opus-in-Ogg)\n");)
                return RB_STREAM_CODEC_UNKNOWN;
            }
        }
    }
    if (peek && peek_len >= 4 && peek[0] == 'O' && peek[1] == 'g' && peek[2] == 'g' && peek[3] == 'S') {
        RADIO_DBG(printf("rb-probe codec: initial byte sniff=OggS final=OGG\n");)
        return RB_STREAM_CODEC_OGG;
    }
    if (info && info->content_type[0]) {
        if (rb_probe_contains_nocase(info->content_type, "audio/mpeg") ||
            rb_probe_contains_nocase(info->content_type, "audio/mp3")) return RB_STREAM_CODEC_MP3;
        if (rb_probe_content_type_is_aac(info->content_type)) return RB_STREAM_CODEC_AAC;
        if (rb_probe_contains_nocase(info->content_type, "audio/opus") ||
            rb_probe_contains_nocase(info->content_type, "audio/x-opus")) return RB_STREAM_CODEC_UNKNOWN;
        if (rb_probe_contains_nocase(info->content_type, "audio/ogg") ||
            rb_probe_contains_nocase(info->content_type, "application/ogg") ||
            rb_probe_contains_nocase(info->content_type, "audio/vorbis") ||
            rb_probe_contains_nocase(info->content_type, "audio/x-vorbis")) return RB_STREAM_CODEC_OGG;
    }
    if (rb_probe_url_has_aac_hint(url))
        return RB_STREAM_CODEC_AAC;
    if (url && rb_probe_contains_nocase(url->path, ".opus"))
        return RB_STREAM_CODEC_UNKNOWN;
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
    case RB_STREAM_PROBE_ERR_TLS_POISONED: return "HTTPS disabled after memory corruption; reboot before using HTTPS.";
    case RB_STREAM_PROBE_ERR_MEM_POISONED: return "Memory corruption detected; restart MintAMP before playing radio.";
    case RB_STREAM_PROBE_ERR_DISABLED: return "Probe/fetch disabled by runtime flag or staged optional-network gate";
    case RB_STREAM_PROBE_ERR_HTTP_STATUS: return "Stream unavailable (server returned an error status)";
    default: return "Stream probe failed";
    }
}

static int rb_probe_stream_url_impl(const char *url, RbStreamInfo *info,
                        unsigned char *peek_buf, int peek_buf_size, int *peek_len);

int rb_probe_stream_probe_test_enabled(void)
{
    Radio_LogRuntimeFlagsOnce();
    return !radio_runtime_flag_enabled("MP3_NO_STREAM_PROBE");
}

int rb_probe_stream_probe_disabled(void)
{
    Radio_LogRuntimeFlagsOnce();
    if (radio_runtime_flag_enabled("MP3_NO_STREAM_PROBE"))
        return 1;
    return 0;
}

int rb_probe_artwork_test_enabled(void)
{
    Radio_LogRuntimeFlagsOnce();
    return !radio_runtime_flag_enabled("MP3_NO_ARTWORK") &&
        !rb_probe_artwork_disabled_for_run;
}

#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
static int rb_probe_optional_network_gate(const char *kind, const char *url)
{
    long active_stream_sessions = 0;
    long active_stream_tasks = 0;
    long open_socket_count = 0;
    long active_ssl_count = 0;
    long active_ssl_ctx_count = 0;
    int worker_idle;

    Radio_GetNetworkStats(&active_stream_sessions, &active_stream_tasks,
        &open_socket_count, &active_ssl_count, &active_ssl_ctx_count);
    worker_idle = Radio_WorkerIsIdle();
    if (!worker_idle || active_stream_sessions != 0 || active_stream_tasks != 0 ||
        open_socket_count != 0 || active_ssl_count != 0) {
        RADIO_DBG(printf("radio-%s: deferred/skipped because optional network gate busy state=%s active_stream_sessions=%ld active_stream_tasks=%ld open_socket_count=%ld active_ssl_count=%ld active_ssl_ctx_count=%ld url=\"%s\"\n",
            kind ? kind : "optional",
            Radio_WorkerStateName(),
            active_stream_sessions,
            active_stream_tasks,
            open_socket_count,
            active_ssl_count,
            active_ssl_ctx_count,
            url ? url : "");)
        return 0;
    }
    RADIO_DBG(printf("radio-%s: optional network gate pass state=%s active_stream_sessions=%ld active_stream_tasks=%ld open_socket_count=%ld active_ssl_count=%ld active_ssl_ctx_count=%ld url=\"%s\"\n",
        kind ? kind : "optional",
        Radio_WorkerStateName(),
        active_stream_sessions,
        active_stream_tasks,
        open_socket_count,
        active_ssl_count,
        active_ssl_ctx_count,
        url ? url : "");)
    return 1;
}
#endif

/* Public entry point.  The opaque transport in radio_stream.c owns all network I/O. */
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
/* rb_probe_stream_url_impl()'s entire body -- transport
 * transport -- touches transport,
 * so per this file's single-net-worker-task rule (see radio_stream.c's
 * Radio_RunOnNetWorker()) it may only run on that one worker task. Nothing
 * inside rb_probe_stream_url_impl() itself needs to change: it already only
 * ever ran in whichever task called rb_probe_stream_url(), and now that is
 * always the worker task, so its existing rb_probe_ensure_amissl()/
 * Radio_AmiSslTaskIsOpener() logic (adopt the shared instance, skip a
 * manual InitAmiSSL() for the opener task) does exactly the right thing
 * unmodified. */
typedef struct RbProbeStreamUrlJobArgs {
    RbProbeRequestSync sync;
    char *url;
    RbStreamInfo info;
    unsigned char *peek_buf;
    int peek_buf_size;
    int peek_len;
    int result;
} RbProbeStreamUrlJobArgs;

static void rb_probe_stream_url_job_free(RbProbeStreamUrlJobArgs *a)
{
    if (!a) return;
    if (a->url) free(a->url);
    if (a->peek_buf) free(a->peek_buf);
    free(a);
}

static void rb_probe_stream_url_job_complete(RbProbeStreamUrlJobArgs *a)
{
    int abandoned;
    if (!a) return;
    rb_probe_request_lock(&a->sync);
    abandoned = (a->sync.state == RB_PROBE_REQ_ABANDONED);
    if (!abandoned)
        a->sync.state = RB_PROBE_REQ_COMPLETED;
    rb_probe_request_unlock(&a->sync);
    if (abandoned)
        rb_probe_stream_url_job_free(a);
}

static void rb_probe_stream_url_job(void *arg)
{
    RbProbeStreamUrlJobArgs *a = (RbProbeStreamUrlJobArgs *)arg;
    int local_peek_len = 0;
    if (!a) return;
    a->result = rb_probe_stream_url_impl(a->url, &a->info, a->peek_buf, a->peek_buf_size, &local_peek_len);
    a->peek_len = local_peek_len;
    rb_probe_stream_url_job_complete(a);
}
#endif

int rb_probe_stream_url(const char *url, RbStreamInfo *info,
                        unsigned char *peek_buf, int peek_buf_size, int *peek_len)
{
    if (rb_probe_stream_probe_disabled()) {
        printf("radio-probe: stream probe disabled by MP3_NO_STREAM_PROBE, direct playback url=\"%s\"\n", url ? url : "");
        if (info) {
            rb_probe_info_init(info);
            rb_probe_set_final_url(info, url ? url : "");
        }
        if (peek_len) *peek_len = 0;
        return RB_STREAM_PROBE_ERR_DISABLED;
    }
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    {
        RbProbeStreamUrlJobArgs *args;
        int result;
        if (!rb_probe_optional_network_gate("probe", url))
            return RB_STREAM_PROBE_ERR_DISABLED;
        args = (RbProbeStreamUrlJobArgs *)calloc(1, sizeof(*args));
        if (!args) return RB_STREAM_PROBE_ERR_CONNECT;
        rb_probe_request_sync_init(&args->sync);
        args->url = rb_probe_dup_string(url ? url : "");
        args->peek_buf_size = peek_buf_size;
        args->peek_buf = peek_buf_size > 0 ? (unsigned char *)malloc((size_t)peek_buf_size) : NULL;
        args->result = RB_STREAM_PROBE_ERR_CONNECT;
        if (!args->url || (peek_buf_size > 0 && !args->peek_buf)) {
            rb_probe_stream_url_job_free(args);
            return RB_STREAM_PROBE_ERR_CONNECT;
        }
        if (!Radio_RunOnNetWorker(rb_probe_stream_url_job, args)) {
            rb_probe_request_lock(&args->sync);
            if (args->sync.state == RB_PROBE_REQ_COMPLETED) {
                result = args->result;
                if (info) *info = args->info;
                if (peek_len) *peek_len = args->peek_len;
                if (peek_buf && args->peek_len > 0)
                    memcpy(peek_buf, args->peek_buf, (size_t)args->peek_len);
                rb_probe_request_unlock(&args->sync);
                rb_probe_stream_url_job_free(args);
                return result;
            }
            args->sync.state = RB_PROBE_REQ_ABANDONED;
            rb_probe_request_unlock(&args->sync);
            return RB_STREAM_PROBE_ERR_CONNECT;
        }
        rb_probe_request_lock(&args->sync);
        result = args->result;
        if (info) *info = args->info;
        if (peek_len) *peek_len = args->peek_len;
        if (peek_buf && args->peek_len > 0)
            memcpy(peek_buf, args->peek_buf, (size_t)args->peek_len);
        rb_probe_request_unlock(&args->sync);
        rb_probe_stream_url_job_free(args);
        if (!rb_probe_optional_network_gate("probe", url))
            return RB_STREAM_PROBE_ERR_CONNECT;
        return result;
    }
#else
    int rc = rb_probe_stream_url_impl(url, info, peek_buf, peek_buf_size, peek_len);
    return rc;
#endif
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
    if (Radio_PlaybackOwnsNetwork()) {
        RADIO_DBG(printf("rb-probe: skipped stream probe while radio playback child owns networking url=\"%s\"\n", url);)
        return RB_STREAM_PROBE_ERR_DISABLED;
    }
    if (Radio_IsMemoryPoisoned()) {
        /* Corrupt heap: no probe of any kind (not even plain HTTP/DNS) may
         * run again this app run -- see docs/amissl-lifecycle-audit.md F3. */
        RADIO_DBG(printf("rb-probe: refused probe after memory poison url=\"%s\"\n", url);)
        return RB_STREAM_PROBE_ERR_MEM_POISONED;
    }
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
        rc = rb_probe_transport_open(&transport, current_url, parsed.host, parsed.port, parsed.isSSL, &info->host_addr_be);
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
n = rb_probe_transport(&transport, (char *)header_buf + total, want);
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
        rb_probe_transport_close_mode(&transport, RB_PROBE_CLOSE_GRACEFUL, info->http_status);
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
n2 = rb_probe_transport(&transport, (char *)peek_buf + *peek_len, want2);
        if (n2 < 0) {
            rb_probe_transport_close(&transport);
            return RB_STREAM_PROBE_ERR_RECV;
        }
        if (n2 == 0) break;
        *peek_len += n2;
    }
    /* Successful 2xx probe completion is a healthy close: send one best-effort
     * close_notify, free SSL/CTX while the socket remains attached, then close
     * the raw socket. */
    rb_probe_transport_close_mode(&transport, RB_PROBE_CLOSE_GRACEFUL, info->http_status);
    info->redirect_count = redirects;
    rc = rb_probe_set_final_url(info, current_url);
    if (rc < 0) {
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
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
#endif
        return RB_STREAM_PROBE_ERR_HLS_UNSUPPORTED;
    }
    if (info->content_type[0] && info->codec == RB_STREAM_CODEC_UNKNOWN) {
        RADIO_DBG(printf("rb-probe cleanup: unsupported cleanup start final_url=%s content_type=%s\n", current_url, info->content_type);)
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
#endif
        RADIO_DBG(printf("rb-probe cleanup: unsupported cleanup end final_state=ERROR codec=unsupported\n");)
        return RB_STREAM_PROBE_ERR_UNSUPPORTED_CONTENT_TYPE;
    }
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
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
    if (Radio_PlaybackOwnsNetwork()) {
        RADIO_DBG(printf("rb-probe: skipped binary fetch while radio playback child owns networking url=\"%s\"\n", url);)
        return RB_STREAM_PROBE_ERR_DISABLED;
    }
    if (Radio_IsMemoryPoisoned()) {
        /* Corrupt heap: no fetch of any kind (favicon/artwork included) may
         * run again this app run -- see docs/amissl-lifecycle-audit.md F3. */
        RADIO_DBG(printf("rb-probe: refused fetch after memory poison url=\"%s\"\n", url);)
        return RB_STREAM_PROBE_ERR_MEM_POISONED;
    }
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
        rc = rb_probe_transport_open_artwork(&transport, current_url, parsed.host, parsed.port, parsed.isSSL);
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
n = rb_probe_transport(&transport, (char *)header_buf + total, want);
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
        rb_probe_transport_close_mode(&transport, RB_PROBE_CLOSE_GRACEFUL, info.http_status);
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
n = rb_probe_transport(&transport, (char *)out_buf + *out_len, want);
        if (n < 0) {
            rb_probe_transport_close(&transport);
            return RB_STREAM_PROBE_ERR_RECV;
        }
        if (n == 0) break;
        *out_len += n;
    }
    /* Completed artwork fetch is a healthy TLS close; fatal read paths above
     * mark the transport poisoned before cleanup. */
    rb_probe_transport_close_mode(&transport, RB_PROBE_CLOSE_GRACEFUL, info.http_status);

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
/* Test mode (MP3_NO_ARTWORK=1 in the environment): refuse every binary fetch
 * (station favicon/artwork) so a radio soak test can run with the GUI's
 * probe/artwork path removed from the equation while still exercising
 * network+SSL+decode+audio cleanup for the actual stream. */
int rb_probe_artwork_disabled(void)
{
    Radio_LogRuntimeFlagsOnce();
    if (rb_probe_artwork_disabled_for_run)
        return 1;
    if (radio_runtime_flag_enabled("MP3_NO_ARTWORK"))
        return 1;
    return 0;
}

#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
typedef struct RbProbeFetchBinaryJobArgs {
    RbProbeRequestSync sync;
    char *url;
    unsigned char *out_buf;
    int out_buf_size;
    int out_len;
    char *out_content_type;
    int out_content_type_size;
    int result;
} RbProbeFetchBinaryJobArgs;

static void rb_probe_fetch_binary_job_free(RbProbeFetchBinaryJobArgs *a)
{
    if (!a) return;
    if (a->url) free(a->url);
    if (a->out_buf) free(a->out_buf);
    if (a->out_content_type) free(a->out_content_type);
    free(a);
}

static void rb_probe_fetch_binary_job_complete(RbProbeFetchBinaryJobArgs *a)
{
    int abandoned;
    if (!a) return;
    rb_probe_request_lock(&a->sync);
    abandoned = (a->sync.state == RB_PROBE_REQ_ABANDONED);
    if (!abandoned)
        a->sync.state = RB_PROBE_REQ_COMPLETED;
    rb_probe_request_unlock(&a->sync);
    if (abandoned)
        rb_probe_fetch_binary_job_free(a);
}

static void rb_probe_fetch_binary_job(void *arg)
{
    RbProbeFetchBinaryJobArgs *a = (RbProbeFetchBinaryJobArgs *)arg;
    int local_len = 0;
    if (!a) return;
    a->result = rb_probe_fetch_binary_impl(a->url, a->out_buf, a->out_buf_size,
        &local_len, a->out_content_type, a->out_content_type_size);
    a->out_len = local_len;
    rb_probe_fetch_binary_job_complete(a);
}
#endif

int rb_probe_fetch_binary(const char *url, unsigned char *out_buf, int out_buf_size,
                        int *out_len, char *out_content_type, int out_content_type_size)
{
    if (rb_probe_artwork_disabled()) {
        if (rb_probe_artwork_disabled_for_run) {
            printf("radio-art: disabled for run after fatal TLS/artwork transport fault\n");
        } else if (radio_runtime_flag_enabled("MP3_NO_ARTWORK")) {
            printf("radio-art: skipped by MP3_NO_ARTWORK\n");
        } else {
            printf("radio-art: optional artwork unavailable url=\"%s\"\n", url ? url : "");
        }
        return RB_STREAM_PROBE_ERR_DISABLED;
    }
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    {
        RbProbeFetchBinaryJobArgs *args;
        int result;
        if (!rb_probe_optional_network_gate("art", url))
            return RB_STREAM_PROBE_ERR_DISABLED;
        args = (RbProbeFetchBinaryJobArgs *)calloc(1, sizeof(*args));
        if (!args) return RB_STREAM_PROBE_ERR_CONNECT;
        rb_probe_request_sync_init(&args->sync);
        args->url = rb_probe_dup_string(url ? url : "");
        args->out_buf_size = out_buf_size;
        args->out_content_type_size = out_content_type_size;
        args->out_buf = out_buf_size > 0 ? (unsigned char *)malloc((size_t)out_buf_size) : NULL;
        args->out_content_type = out_content_type_size > 0 ? (char *)malloc((size_t)out_content_type_size) : NULL;
        args->result = RB_STREAM_PROBE_ERR_CONNECT;
        if (!args->url || (out_buf_size > 0 && !args->out_buf) ||
            (out_content_type_size > 0 && !args->out_content_type)) {
            rb_probe_fetch_binary_job_free(args);
            return RB_STREAM_PROBE_ERR_CONNECT;
        }
        if (!Radio_RunOnNetWorker(rb_probe_fetch_binary_job, args)) {
            rb_probe_request_lock(&args->sync);
            if (args->sync.state == RB_PROBE_REQ_COMPLETED) {
                result = args->result;
                if (out_len) *out_len = args->out_len;
                if (out_buf && args->out_len > 0) memcpy(out_buf, args->out_buf, (size_t)args->out_len);
                if (out_content_type && out_content_type_size > 0 && args->out_content_type) {
                    strncpy(out_content_type, args->out_content_type, (size_t)out_content_type_size - 1);
                    out_content_type[out_content_type_size - 1] = '\0';
                }
                rb_probe_request_unlock(&args->sync);
                rb_probe_fetch_binary_job_free(args);
                return result;
            }
            args->sync.state = RB_PROBE_REQ_ABANDONED;
            rb_probe_request_unlock(&args->sync);
            return RB_STREAM_PROBE_ERR_CONNECT;
        }
        rb_probe_request_lock(&args->sync);
        result = args->result;
        if (out_len) *out_len = args->out_len;
        if (out_buf && args->out_len > 0) memcpy(out_buf, args->out_buf, (size_t)args->out_len);
        if (out_content_type && out_content_type_size > 0 && args->out_content_type) {
            strncpy(out_content_type, args->out_content_type, (size_t)out_content_type_size - 1);
            out_content_type[out_content_type_size - 1] = '\0';
        }
        rb_probe_request_unlock(&args->sync);
        rb_probe_fetch_binary_job_free(args);
        if (rb_probe_open_socket_count != 0 || rb_probe_active_ssl_count != 0 || Radio_IsTlsPoisoned()) {
            rb_probe_artwork_disabled_for_run = 1;
            return RB_STREAM_PROBE_ERR_CONNECT;
        }
        if (!rb_probe_optional_network_gate("art", url)) return RB_STREAM_PROBE_ERR_CONNECT;
        return result;
    }
#else
    {
        int rc = rb_probe_fetch_binary_impl(url, out_buf, out_buf_size, out_len,
                                             out_content_type, out_content_type_size);
        return rc;
    }
#endif
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
