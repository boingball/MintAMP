/*
 * Small fixed-buffer HTTP/ICY stream probe.
 *
 * Test harness build:
 *   gcc -std=gnu89 -Wall -Wextra -DRB_STREAM_PROBE_TEST radio_stream_probe.c -o /tmp/rb_stream_probe_test
 */

#include "radio_stream_probe.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#if defined(AMIGA_M68K)
#include <exec/types.h>
#include <exec/libraries.h>
#include <proto/exec.h>
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
#endif /* HAVE_AMISSL */
#else
#include <unistd.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#define RB_PROBE_SOCKET int
#define RB_PROBE_INVALID_SOCKET (-1)
#define rb_probe_close_socket(s) close(s)
#endif

#define RB_PROBE_DEFAULT_PORT 80
#define RB_PROBE_MAX_HOST 256
#define RB_PROBE_MAX_PATH 512
#define RB_PROBE_MAX_REQUEST 1024
#define RB_PROBE_HEADER_BUF 4096
#define RB_PROBE_READ_CHUNK 512
#define RB_PROBE_MAX_REDIRECTS 3
#define RB_PROBE_MAX_URL 512

typedef struct RbProbeUrl {
    char host[RB_PROBE_MAX_HOST];
    char path[RB_PROBE_MAX_PATH];
    int port;
    int isSSL;
} RbProbeUrl;

typedef struct RbProbeTransport {
    RB_PROBE_SOCKET sock;
    int isSSL;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    SSL *ssl;
    SSL_CTX *ctx;
    int sslHandshakeDone;
#endif
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
    if (!SocketBase) {
        SocketBase = OpenLibrary("bsdsocket.library", 4);
        if (!SocketBase) return -1;
    }
    if (AmiSSLBase && rb_probe_amissl_initialized) return 0;
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
    if (InitAmiSSL(AmiSSL_SocketBase, (ULONG)SocketBase,
                   AmiSSL_ErrNoPtr, (ULONG)&errno,
                   TAG_DONE) != 0) {
        CloseAmiSSL(); AmiSSLBase = NULL; AmiSSLExtBase = NULL; return -1;
    }
    rb_probe_amissl_initialized = 1;
    return 0;
}

static void rb_probe_cleanup_amissl(void)
{
    if (rb_probe_amissl_initialized) {
        CleanupAmiSSL(TAG_DONE);
        rb_probe_amissl_initialized = 0;
    }
    if (AmiSSLBase) {
        CloseAmiSSL();
        AmiSSLBase = NULL;
        AmiSSLExtBase = NULL;
    }
    if (AmiSSLMasterBase) {
        CloseLibrary(AmiSSLMasterBase);
        AmiSSLMasterBase = NULL;
    }
}

#endif

static int rb_probe_transport_open(RbProbeTransport *transport, const char *host, int port, int use_ssl, RbStreamInfo *info)
{
    struct hostent *he;
    struct sockaddr_in sa;

    if (!transport || !host) return RB_STREAM_PROBE_ERR_BAD_ARG;
    transport->sock = RB_PROBE_INVALID_SOCKET;
    transport->isSSL = 0;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    transport->ssl = NULL;
    transport->ctx = NULL;
    transport->sslHandshakeDone = 0;
#endif

#if defined(AMIGA_M68K) && !defined(RB_STREAM_PROBE_EXTERNAL_SOCKETBASE)
    if (!SocketBase) {
        SocketBase = OpenLibrary("bsdsocket.library", 4);
        if (!SocketBase) return RB_STREAM_PROBE_ERR_CONNECT;
    }
#endif
    he = gethostbyname(host);
    if (!he || !he->h_addr_list || !he->h_addr_list[0]) return RB_STREAM_PROBE_ERR_DNS;
    transport->sock = socket(AF_INET, SOCK_STREAM, 0);
    if (transport->sock == RB_PROBE_INVALID_SOCKET) return RB_STREAM_PROBE_ERR_CONNECT;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    memcpy(&sa.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
    if (info) {
        info->have_host_addr = 1;
        memcpy(&info->host_addr_be, he->h_addr_list[0], sizeof(info->host_addr_be));
        printf("rb-probe DNS: resolved %s -> %s\n", host, inet_ntoa(sa.sin_addr));
    }
    if (connect(transport->sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        rb_probe_close_socket(transport->sock);
        transport->sock = RB_PROBE_INVALID_SOCKET;
        return RB_STREAM_PROBE_ERR_CONNECT;
    }
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    if (use_ssl) {
        int ssl_connect_rc;
        int ssl_error;
        unsigned long ssl_lib_error;
        char ssl_error_buf[160];
        int sni_set;

        ssl_connect_rc = 0;
        ssl_error = 0;
        ssl_lib_error = 0;
        ssl_error_buf[0] = '\0';
        sni_set = 0;
        if (rb_probe_ensure_amissl() != 0) {
            rb_probe_close_socket(transport->sock);
            transport->sock = RB_PROBE_INVALID_SOCKET;
            return RB_STREAM_PROBE_ERR_CONNECT;
        }
        transport->ctx = SSL_CTX_new(SSLv23_client_method());
        if (!transport->ctx) {
            rb_probe_close_socket(transport->sock);
            transport->sock = RB_PROBE_INVALID_SOCKET;
            return RB_STREAM_PROBE_ERR_CONNECT;
        }
        /* No CA bundle on classic AmigaOS; skip cert verification for streams. */
        SSL_CTX_set_verify(transport->ctx, SSL_VERIFY_NONE, NULL);
        transport->ssl = SSL_new(transport->ctx);
        if (!transport->ssl) {
            SSL_CTX_free(transport->ctx); transport->ctx = NULL;
            rb_probe_close_socket(transport->sock);
            transport->sock = RB_PROBE_INVALID_SOCKET;
            return RB_STREAM_PROBE_ERR_CONNECT;
        }
        SSL_set_fd(transport->ssl, (int)transport->sock);
#ifdef SSL_CTRL_SET_TLSEXT_HOSTNAME
        sni_set = (SSL_set_tlsext_host_name(transport->ssl, host) == 1);
#else
        sni_set = -1;
#endif
        printf("rb-probe TLS: host=%s port=%d sni=%s verify=disabled method=SSLv23_client_method\n",
               host, port, sni_set > 0 ? host : (sni_set == 0 ? "not-set" : "unavailable"));
        ssl_connect_rc = SSL_connect(transport->ssl);
        if (ssl_connect_rc != 1) {
            ssl_error = SSL_get_error(transport->ssl, ssl_connect_rc);
            ssl_lib_error = ERR_get_error();
            if (ssl_lib_error != 0)
                ERR_error_string_n(ssl_lib_error, ssl_error_buf, sizeof(ssl_error_buf));
            printf("rb-probe TLS: SSL_connect rc=%d SSL_get_error=%d error=\"%s\" verify=disabled method=SSLv23_client_method\n",
                   ssl_connect_rc, ssl_error, ssl_error_buf[0] ? ssl_error_buf : "none");
            SSL_free(transport->ssl); transport->ssl = NULL;
            SSL_CTX_free(transport->ctx); transport->ctx = NULL;
            rb_probe_close_socket(transport->sock);
            transport->sock = RB_PROBE_INVALID_SOCKET;
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

static void rb_probe_transport_close(RbProbeTransport *transport)
{
    if (transport && transport->sock != RB_PROBE_INVALID_SOCKET) {
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
        if (transport->ssl && transport->sslHandshakeDone) {
            SSL_shutdown(transport->ssl);
        }
        if (transport->ssl) {
            SSL_free(transport->ssl);
            transport->ssl = NULL;
            transport->sslHandshakeDone = 0;
        }
        if (transport->ctx) {
            SSL_CTX_free(transport->ctx);
            transport->ctx = NULL;
        }
#endif
        rb_probe_close_socket(transport->sock);
        transport->sock = RB_PROBE_INVALID_SOCKET;
    }
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

static RbStreamCodec rb_probe_detect_codec(const RbProbeUrl *url, const RbStreamInfo *info,
                                           const unsigned char *peek, int peek_len)
{
    if (peek && peek_len >= 3 && peek[0] == 'I' && peek[1] == 'D' && peek[2] == '3') {
        printf("rb-probe codec: initial byte sniff=ID3 final=MP3\n");
        return RB_STREAM_CODEC_MP3;
    }
    if (peek && peek_len >= 2 && peek[0] == 0xff && (peek[1] & 0xe0) == 0xe0 &&
        peek[1] != 0xf1 && peek[1] != 0xf9) {
        printf("rb-probe codec: initial byte sniff=MPEG frame sync final=MP3\n");
        return RB_STREAM_CODEC_MP3;
    }
    if (peek && peek_len >= 2 && peek[0] == 0xff && (peek[1] == 0xf1 || peek[1] == 0xf9)) {
        printf("rb-probe codec: initial byte sniff=ADTS final=AAC\n");
        return RB_STREAM_CODEC_AAC;
    }
    if (info && info->content_type[0]) {
        if (rb_probe_contains_nocase(info->content_type, "audio/mpeg") ||
            rb_probe_contains_nocase(info->content_type, "audio/mp3")) return RB_STREAM_CODEC_MP3;
        if (rb_probe_contains_nocase(info->content_type, "audio/aac") ||
            rb_probe_contains_nocase(info->content_type, "audio/aacp") ||
            rb_probe_contains_nocase(info->content_type, "audio/x-aac")) return RB_STREAM_CODEC_AAC;
    }
    if (url && (rb_probe_contains_nocase(url->path, ".aac") || rb_probe_contains_nocase(url->path, ".aacp")))
        return RB_STREAM_CODEC_AAC;
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
    case RB_STREAM_PROBE_ERR_HTTP_STATUS: return "Stream probe failed: HTTP status was not successful";
    default: return "Stream probe failed";
    }
}

int rb_probe_stream_url(const char *url, RbStreamInfo *info,
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
        rc = rb_probe_transport_open(&transport, parsed.host, parsed.port, parsed.isSSL, info);
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
            if (transport.isSSL && transport.ssl)
                n = (int)SSL_read(transport.ssl, (char *)header_buf + total, want);
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
        if (!rb_probe_is_redirect_status(info->http_status) &&
            (info->http_status < 200 || info->http_status > 299)) {
            rb_probe_transport_close(&transport);
            return RB_STREAM_PROBE_ERR_HTTP_STATUS;
        }
        if (!rb_probe_is_redirect_status(info->http_status)) break;
        rb_probe_transport_close(&transport);
        *peek_len = 0;
        if (!location[0]) return RB_STREAM_PROBE_ERR_BAD_URL;
        if (redirects >= RB_PROBE_MAX_REDIRECTS) return RB_STREAM_PROBE_ERR_TOO_MANY_REDIRECTS;
        rc = rb_probe_resolve_location(&parsed, location, next_url, (int)sizeof(next_url));
        if (rc < 0) return rc;
        redirects++;
        rc = rb_probe_copy_string(current_url, (int)sizeof(current_url), next_url);
        if (rc < 0) return rc;
    }
    while (*peek_len < peek_buf_size) {
        int want2;
        int n2;

        want2 = peek_buf_size - *peek_len;
        if (want2 > RB_PROBE_READ_CHUNK) want2 = RB_PROBE_READ_CHUNK;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
        if (transport.isSSL && transport.ssl)
            n2 = (int)SSL_read(transport.ssl, (char *)peek_buf + *peek_len, want2);
        else
#endif
        n2 = (int)recv(transport.sock, (char *)peek_buf + *peek_len, want2, 0);
        if (n2 < 0) {
            rb_probe_transport_close(&transport);
            return RB_STREAM_PROBE_ERR_RECV;
        }
        if (n2 == 0) break;
        *peek_len += n2;
    }
    rb_probe_transport_close(&transport);
    info->redirect_count = redirects;
    rc = rb_probe_set_final_url(info, current_url);
    if (rc < 0) {
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
        rb_probe_cleanup_amissl();
#endif
        return rc;
    }
    printf("rb-probe codec: final URL=%s content-type=%s URL codec hint=%s initial-bytes=%d\n",
           current_url, info->content_type, rb_probe_url_has_mp3_hint(&parsed) ? "MP3" : "none", *peek_len);
    info->codec = rb_probe_detect_codec(&parsed, info, peek_buf, *peek_len);
    printf("rb-probe codec: final selected codec=%s\n",
           info->codec == RB_STREAM_CODEC_MP3 ? "MP3" : (info->codec == RB_STREAM_CODEC_AAC ? "AAC" : "unsupported"));
    if (rb_probe_is_hls(&parsed, info)) {
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
        rb_probe_cleanup_amissl();
#endif
        return RB_STREAM_PROBE_ERR_HLS_UNSUPPORTED;
    }
    if (info->content_type[0] && info->codec == RB_STREAM_CODEC_UNKNOWN) {
        printf("rb-probe cleanup: unsupported cleanup start final_url=%s content_type=%s\n", current_url, info->content_type);
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
        rb_probe_cleanup_amissl();
#endif
        printf("rb-probe cleanup: unsupported cleanup end final_state=ERROR codec=unsupported\n");
        return RB_STREAM_PROBE_ERR_UNSUPPORTED_CONTENT_TYPE;
    }
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    rb_probe_cleanup_amissl();
#endif
    return RB_STREAM_PROBE_OK;
}

#ifdef RB_STREAM_PROBE_TEST
static const char *rb_probe_codec_name(RbStreamCodec codec)
{
    switch (codec) {
    case RB_STREAM_CODEC_MP3: return "MP3";
    case RB_STREAM_CODEC_AAC: return "AAC";
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
    if (!rb_probe_url_looks_hls("http://example.com/live.m3u8")) return 7;
    printf("rb-probe selftest: ok\n");
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
    printf("content type: %s\n", info.content_type);
    printf("icy name: %s\n", info.icy_name);
    printf("icy bitrate: %d\n", info.icy_br);
    printf("icy metaint: %d\n", info.icy_metaint);
    printf("detected codec: %s\n", rb_probe_codec_name(info.codec));
    printf("peek bytes: %d\n", peek_len);
    return 0;
}
#endif
