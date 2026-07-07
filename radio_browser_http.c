/*
 * Small fixed-buffer HTTP GET helper for Radio Browser.
 *
 * Test harness build:
 *   gcc -std=gnu89 -Wall -Wextra -DRB_HTTP_TEST radio_browser_http.c radio_browser_url.c radio_browser_json.c -o /tmp/rb_http_test
 *   gcc -std=gnu89 -Wall -Wextra -DRB_RADIO_E2E_TEST radio_browser_url.c radio_browser_http.c radio_browser_json.c radio_stream_probe.c -o /tmp/rb_radio_e2e_test
 */

#include "radio_browser_http.h"
#include "radio_debug.h"
#include "radio_stream.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

#if defined(AMIGA_M68K)
#include <stdarg.h>
#include <exec/semaphores.h>
#include <proto/exec.h>
/* Same cross-task shared-stdout race as radio_stream.c (see radio_debug.h's
 * RADIO_DBG comment for the field evidence): favicon/artwork HTTP fetches
 * here run concurrently with radio_stream.c/amiga_mp3dec.c's own output.
 * Redirect every printf() in this file through the same shared
 * radio_console_lock. */
extern struct SignalSemaphore radio_console_lock;
static int RadioBrowserHttpLockedPrintf(const char *fmt, ...)
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
#define printf RadioBrowserHttpLockedPrintf
#include <exec/types.h>
#include <exec/libraries.h>
#include <proto/bsdsocket.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/ioctl.h>
#ifndef RB_HTTP_EXTERNAL_SOCKETBASE
/* Weak so this module can link either standalone or beside radio_stream.c,
 * which also provides SocketBase for bsdsocket.library builds. */
struct Library *SocketBase __attribute__((weak));
#endif
#define RB_HTTP_SOCKET long
#define RB_HTTP_INVALID_SOCKET (-1)
#define rb_http_close_socket(s) CloseSocket(s)
#else
#include <unistd.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <fcntl.h>
#define RB_HTTP_SOCKET int
#define RB_HTTP_INVALID_SOCKET (-1)
#define rb_http_close_socket(s) close(s)
#endif

#define RB_HTTP_PORT 80
#define RB_HTTP_READ_CHUNK 512
#define RB_HTTP_MAX_REQUEST 512
#define RB_HTTP_CONNECT_TIMEOUT_SEC 8
#define RB_HTTP_IO_TIMEOUT_SEC 8

typedef struct RbHttpTransport {
    RB_HTTP_SOCKET sock;
} RbHttpTransport;

#if defined(AMIGA_M68K) && !defined(RB_HTTP_EXTERNAL_SOCKETBASE)
/* bsdsocket.library is now opened once by Radio_NetworkInit() at app startup
 * and closed once by Radio_NetworkShutdown() at app exit (see radio_stream.c)
 * instead of per-request -- this is now a deliberate no-op, kept so its call
 * sites below don't all need to be removed individually. */
static void rb_http_release_socketbase(void)
{
}
#endif


static int rb_http_set_nonblocking(RB_HTTP_SOCKET sock, int enabled)
{
#if defined(AMIGA_M68K)
    long mode = enabled ? 1 : 0;
    return IoctlSocket(sock, FIONBIO, (char *)&mode);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) return -1;
    if (enabled) flags |= O_NONBLOCK;
    else flags &= ~O_NONBLOCK;
    return fcntl(sock, F_SETFL, flags);
#endif
}

static int rb_http_wait_socket(RB_HTTP_SOCKET sock, int for_write, int timeout_sec)
{
    fd_set rfds;
    fd_set wfds;
    struct timeval tv;
    int rc;

    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    if (for_write) FD_SET(sock, &wfds);
    else FD_SET(sock, &rfds);
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
#if defined(AMIGA_M68K)
    rc = WaitSelect((int)sock + 1,
        for_write ? NULL : &rfds,
        for_write ? &wfds : NULL,
        NULL,
        &tv,
        NULL);
#else
    rc = select((int)sock + 1, for_write ? NULL : &rfds, for_write ? &wfds : NULL, NULL, &tv);
#endif
    if (rc == 0) return RB_HTTP_ERR_TIMEOUT;
    if (rc < 0) return for_write ? RB_HTTP_ERR_CONNECT : RB_HTTP_ERR_READ;
    return 0;
}

static int rb_http_append(char *out, int out_size, int *pos, const char *text)
{
    if (!out || !pos || out_size <= 0 || !text) return RB_HTTP_ERR_BAD_ARG;
    while (*text) {
        if (*pos >= out_size - 1) return RB_HTTP_ERR_BODY_TOO_BIG;
        out[*pos] = *text;
        (*pos)++;
        out[*pos] = '\0';
        text++;
    }
    return 0;
}

static int rb_http_build_request(char *out, int out_size,
                                 const char *host, const char *path)
{
    int pos;
    int rc;

    if (!out || out_size <= 0 || !host || !*host || !path || !*path)
        return RB_HTTP_ERR_BAD_ARG;
    out[0] = '\0';
    pos = 0;
    rc = rb_http_append(out, out_size, &pos, "GET ");
    if (rc < 0) return rc;
    rc = rb_http_append(out, out_size, &pos, path);
    if (rc < 0) return rc;
    rc = rb_http_append(out, out_size, &pos, " HTTP/1.1\r\nHost: ");
    if (rc < 0) return rc;
    rc = rb_http_append(out, out_size, &pos, host);
    if (rc < 0) return rc;
    return rb_http_append(out, out_size, &pos,
        "\r\nUser-Agent: BoingPlayer/0.1 AmigaOS\r\n"
        "Accept: application/json\r\n"
        "Connection: close\r\n\r\n");
}

static int rb_http_transport_open(RbHttpTransport *transport, const char *host, int port)
{
    const struct hostent *he;
    struct sockaddr_in sa;
    int rc;

    if (!transport || !host) return RB_HTTP_ERR_BAD_ARG;
    transport->sock = RB_HTTP_INVALID_SOCKET;

#if defined(AMIGA_M68K) && !defined(RB_HTTP_EXTERNAL_SOCKETBASE)
    if (!SocketBase) {
        /* Adopt the app-wide bsdsocket.library opened by Radio_NetworkInit()
         * first: this file's weak SocketBase does not reliably merge with
         * radio_stream.c's strong definition under the m68k hunk linker (see
         * the matching fix in radio_stream_probe.c), so without this the
         * search path silently ran on its own second bsdsocket instance. */
        void *shared_socket = NULL;
        Radio_GetNetworkBases(&shared_socket, NULL, NULL);
        if (shared_socket) SocketBase = (struct Library *)shared_socket;
    }
    if (!SocketBase) {
        SocketBase = OpenLibrary("bsdsocket.library", 4);
        if (!SocketBase) return RB_HTTP_ERR_CONNECT;
    }
#endif

    /* Amiga bsdsocket headers declare gethostbyname() with a mutable
     * name argument even though the call only reads it. Cast at the call
     * site so const-correct callers do not trigger -Wdiscarded-qualifiers. */
    he = gethostbyname((char *)host);
    if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
#if defined(AMIGA_M68K) && !defined(RB_HTTP_EXTERNAL_SOCKETBASE)
        rb_http_release_socketbase();
#endif
        return RB_HTTP_ERR_DNS;
    }

    transport->sock = socket(AF_INET, SOCK_STREAM, 0);
    if (transport->sock == RB_HTTP_INVALID_SOCKET) {
#if defined(AMIGA_M68K) && !defined(RB_HTTP_EXTERNAL_SOCKETBASE)
        rb_http_release_socketbase();
#endif
        return RB_HTTP_ERR_CONNECT;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    memcpy(&sa.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    rb_http_set_nonblocking(transport->sock, 1);
    if (connect(transport->sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        rc = rb_http_wait_socket(transport->sock, 1, RB_HTTP_CONNECT_TIMEOUT_SEC);
        if (rc < 0) {
            rb_http_close_socket(transport->sock);
            transport->sock = RB_HTTP_INVALID_SOCKET;
#if defined(AMIGA_M68K) && !defined(RB_HTTP_EXTERNAL_SOCKETBASE)
            rb_http_release_socketbase();
#endif
            return rc;
        }
    }
    return 0;
}

static void rb_http_transport_close(RbHttpTransport *transport)
{
    if (transport && transport->sock != RB_HTTP_INVALID_SOCKET) {
        rb_http_close_socket(transport->sock);
        transport->sock = RB_HTTP_INVALID_SOCKET;
    }
#if defined(AMIGA_M68K) && !defined(RB_HTTP_EXTERNAL_SOCKETBASE)
    rb_http_release_socketbase();
#endif
}

static int rb_http_transport_send_all(RbHttpTransport *transport,
                                      const char *buf, int len)
{
    int sent;

    if (!transport || transport->sock == RB_HTTP_INVALID_SOCKET || !buf || len < 0)
        return RB_HTTP_ERR_BAD_ARG;
    sent = 0;
    while (sent < len) {
        int n;
        {
            int wrc = rb_http_wait_socket(transport->sock, 1, RB_HTTP_IO_TIMEOUT_SEC);
            if (wrc < 0) return wrc;
        }
        n = (int)send(transport->sock, (char *)buf + sent, len - sent, 0);
        if (n <= 0) return RB_HTTP_ERR_SEND;
        sent += n;
    }
    return 0;
}

static int rb_http_find_headers_end(const char *buf, int len)
{
    int i;

    for (i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n') return i + 4;
    }
    return -1;
}

static int rb_http_status_is_2xx(const char *response, int len)
{
    int code;

    if (!response || len < 12) return 0;
    if (strncmp(response, "HTTP/", 5) != 0) return 0;
    code = 0;
    if (sscanf(response, "%*s %d", &code) != 1) return 0;
    return code >= 200 && code < 300;
}

static int rb_http_ascii_starts_nocase(const char *s, int s_len, const char *prefix)
{
    int i;

    for (i = 0; prefix[i]; i++) {
        unsigned char cs, cp;
        if (i >= s_len) return 0;
        cs = (unsigned char)s[i];
        cp = (unsigned char)prefix[i];
        if (cs >= 'a' && cs <= 'z') cs = (unsigned char)(cs - 'a' + 'A');
        if (cp >= 'a' && cp <= 'z') cp = (unsigned char)(cp - 'a' + 'A');
        if (cs != cp) return 0;
    }
    return 1;
}

/* Pulls "Content-Type: ..." out of the raw header block before it gets
 * discarded, so callers that care about the response's media type (e.g. the
 * radio favicon fetch) don't need their own header parser. */
static void rb_http_extract_content_type(const char *headers, int header_len,
                                          char *out, int out_size)
{
    const char *line;
    const char *end = headers + header_len;

    if (out && out_size > 0) out[0] = '\0';
    if (!headers || header_len <= 0 || !out || out_size <= 0) return;

    line = headers;
    while (line < end) {
        const char *nl = line;
        int line_len;
        while (nl < end && *nl != '\n') nl++;
        line_len = (int)(nl - line);
        if (line_len > 0 && line[line_len - 1] == '\r') line_len--;
        if (line_len > 13 && rb_http_ascii_starts_nocase(line, line_len, "Content-Type:")) {
            const char *v = line + 13;
            int v_len = line_len - 13;
            while (v_len > 0 && *v == ' ') { v++; v_len--; }
            if (v_len > out_size - 1) v_len = out_size - 1;
            memcpy(out, v, (size_t)v_len);
            out[v_len] = '\0';
            return;
        }
        line = nl + 1;
    }
}

/* Some Radio Browser mirrors switch to "Transfer-Encoding: chunked" once
 * the response is large enough that they don't buffer it to compute a
 * Content-Length up front -- observed in practice once the station search
 * "limit" grows past what fits in one small response (small limits like 10
 * stay under that threshold and use Content-Length, larger ones like 50
 * don't).  This client only ever did a raw byte-stream read with no
 * framing awareness, so a chunked response's hex chunk-size lines ended up
 * embedded as garbage in what was handed to the JSON parser -- which
 * correctly rejected it as malformed JSON ("Parse failed"), even though
 * every byte had actually arrived intact. */
static int rb_http_is_chunked(const char *headers, int header_len)
{
    const char *line = headers;
    const char *end = headers + header_len;

    if (!headers || header_len <= 0) return 0;
    while (line < end) {
        const char *nl = line;
        int line_len;
        while (nl < end && *nl != '\n') nl++;
        line_len = (int)(nl - line);
        if (line_len > 0 && line[line_len - 1] == '\r') line_len--;
        if (line_len > 18 && rb_http_ascii_starts_nocase(line, line_len, "Transfer-Encoding:")) {
            const char *v = line + 18;
            int v_len = line_len - 18;
            while (v_len > 0 && *v == ' ') { v++; v_len--; }
            if (v_len >= 7 && rb_http_ascii_starts_nocase(v, v_len, "chunked"))
                return 1;
        }
        line = nl + 1;
    }
    return 0;
}

/* Decodes RFC 7230 chunked transfer-coding in place (the dechunked body is
 * always shorter than or equal to its chunked encoding, so compacting
 * toward the front of the same buffer is safe).  Returns the dechunked
 * length, or -1 on malformed chunk framing.  Trailer headers after the
 * final 0-length chunk are ignored; the JSON body never needs them. */
static int rb_http_dechunk(unsigned char *body, int body_len)
{
    int read_pos = 0;
    int write_pos = 0;

    for (;;) {
        int chunk_size = 0;
        int digits = 0;

        while (read_pos < body_len) {
            unsigned char c = body[read_pos];
            int v;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            else break;
            if (chunk_size > (0x7fffffff - v) / 16) return -1;
            chunk_size = chunk_size * 16 + v;
            digits++;
            read_pos++;
        }
        if (digits == 0) return -1;
        while (read_pos < body_len && body[read_pos] != '\r' && body[read_pos] != '\n') read_pos++;
        if (read_pos + 1 >= body_len || body[read_pos] != '\r' || body[read_pos + 1] != '\n') return -1;
        read_pos += 2;
        if (chunk_size == 0) return write_pos;
        if (chunk_size > body_len - read_pos) return -1;
        if (write_pos != read_pos) memmove(body + write_pos, body + read_pos, (size_t)chunk_size);
        write_pos += chunk_size;
        read_pos += chunk_size;
        if (read_pos + 1 >= body_len || body[read_pos] != '\r' || body[read_pos + 1] != '\n') return -1;
        read_pos += 2;
    }
}

static int rb_http_get_binary_impl(const char *host, const char *path,
                       unsigned char *out_body, int out_body_size,
                       char *out_content_type, int out_content_type_size)
{
    RbHttpTransport transport;
    char request[RB_HTTP_MAX_REQUEST];
    int request_len;
    int len;
    int rc;
    int header_end;
    int body_len;

    if (out_content_type && out_content_type_size > 0) out_content_type[0] = '\0';
    if (!host || !path || !out_body || out_body_size <= 0) return RB_HTTP_ERR_BAD_ARG;
    out_body[0] = '\0';
    if (Radio_PlaybackOwnsNetwork()) {
        RADIO_DBG(printf("radio-browser-http: skipped HTTP request while radio playback child owns networking host=%s path=%s\n", host, path);)
        return RB_HTTP_ERR_CONNECT;
    }

    rc = rb_http_build_request(request, (int)sizeof(request), host, path);
    if (rc < 0) return rc;
    request_len = (int)strlen(request);

    rc = rb_http_transport_open(&transport, host, RB_HTTP_PORT);
    if (rc < 0) return rc;

    rc = rb_http_transport_send_all(&transport, request, request_len);
    if (rc < 0) {
        rb_http_transport_close(&transport);
        return rc;
    }

    len = 0;
    while (len < out_body_size - 1) {
        int want;
        int n;

        want = out_body_size - 1 - len;
        if (want > RB_HTTP_READ_CHUNK) want = RB_HTTP_READ_CHUNK;
        rc = rb_http_wait_socket(transport.sock, 0, RB_HTTP_IO_TIMEOUT_SEC);
        if (rc < 0) {
            rb_http_transport_close(&transport);
            out_body[0] = '\0';
            return rc;
        }
        n = (int)recv(transport.sock, (char *)out_body + len, want, 0);
        if (n < 0) {
            rb_http_transport_close(&transport);
            out_body[0] = '\0';
            return RB_HTTP_ERR_READ;
        }
        if (n == 0) break;
        len += n;
    }
    if (len >= out_body_size - 1) {
        char tmp;
        int n;

        n = (int)recv(transport.sock, &tmp, 1, 0);
        if (n > 0) {
            rb_http_transport_close(&transport);
            out_body[0] = '\0';
            return RB_HTTP_ERR_BODY_TOO_BIG;
        }
        if (n < 0) {
            rb_http_transport_close(&transport);
            out_body[0] = '\0';
            return RB_HTTP_ERR_READ;
        }
    }
    rb_http_transport_close(&transport);
    out_body[len] = '\0';

    header_end = rb_http_find_headers_end((const char *)out_body, len);
    if (header_end < 0) {
        out_body[0] = '\0';
        return RB_HTTP_ERR_NO_HEADERS;
    }
    if (!rb_http_status_is_2xx((const char *)out_body, len)) {
        out_body[0] = '\0';
        return RB_HTTP_ERR_STATUS;
    }
    if (out_content_type && out_content_type_size > 0)
        rb_http_extract_content_type((const char *)out_body, header_end,
                                      out_content_type, out_content_type_size);

    body_len = len - header_end;
    if (body_len >= out_body_size) {
        out_body[0] = '\0';
        return RB_HTTP_ERR_BODY_TOO_BIG;
    }
    if (rb_http_is_chunked((const char *)out_body, header_end)) {
        int dechunked_len = rb_http_dechunk(out_body + header_end, body_len);
        if (dechunked_len < 0) {
            out_body[0] = '\0';
            return RB_HTTP_ERR_CHUNKED;
        }
        body_len = dechunked_len;
    }
    memmove(out_body, out_body + header_end, (size_t)body_len);
    out_body[body_len] = '\0';
    return body_len;
}

int rb_http_get_binary(const char *host, const char *path,
                       unsigned char *out_body, int out_body_size)
{
    return rb_http_get_binary_impl(host, path, out_body, out_body_size, NULL, 0);
}

int rb_http_get_binary_with_type(const char *host, const char *path,
                       unsigned char *out_body, int out_body_size,
                       char *out_content_type, int out_content_type_size)
{
    return rb_http_get_binary_impl(host, path, out_body, out_body_size,
                                    out_content_type, out_content_type_size);
}

int rb_http_get_json(const char *host, const char *path,
                     char *out_body, int out_body_size)
{
    int body_len;

    body_len = rb_http_get_binary(host, path, (unsigned char *)out_body, out_body_size);
    if (body_len < 0) return body_len;
    return 0;
}

#ifdef RB_HTTP_TEST
#include "radio_browser_url.h"
#include "radio_browser_json.h"

#define RB_HTTP_TEST_BODY_SIZE 65536

int main(void)
{
    static char path[512];
    static char body[RB_HTTP_TEST_BODY_SIZE];
    RadioBrowserStation stations[RB_MAX_STATIONS];
    int rc;
    int count;
    int i;

    rc = rb_build_station_search_path(path, (int)sizeof(path), 0, 0, "MP3", 0, -1, 10, 0);
    if (rc < 0) {
        printf("build path failed: %d\n", rc);
        return 1;
    }

    rc = rb_http_get_json("de1.api.radio-browser.info", path, body, (int)sizeof(body));
    if (rc < 0) {
        printf("http get failed: %d\n", rc);
        return 1;
    }

    count = rb_parse_stations_json(body, stations, RB_MAX_STATIONS);
    printf("station count: %d\n", count);
    for (i = 0; i < count; i++) {
        printf("%d: %s\n", i + 1, rb_station_play_url(&stations[i]));
    }
    return 0;
}
#endif

#ifdef RB_RADIO_E2E_TEST
#include "radio_browser_url.h"
#include "radio_browser_json.h"
#include "radio_stream_probe.h"

#define RB_RADIO_E2E_HOST "de1.api.radio-browser.info"
#define RB_RADIO_E2E_BODY_SIZE 131072
#define RB_RADIO_E2E_PEEK_SIZE 512
#define RB_RADIO_E2E_BODY_PREVIEW 200

static const char *rb_radio_e2e_codec_name(RbStreamCodec codec)
{
    switch (codec) {
    case RB_STREAM_CODEC_MP3: return "MP3";
    case RB_STREAM_CODEC_AAC: return "AAC";
    default: return "unknown";
    }
}

static int rb_radio_e2e_is_http_url(const char *url)
{
    return url && strncmp(url, "http://", 7) == 0;
}

static int rb_radio_e2e_is_https_url(const char *url)
{
    return url && strncmp(url, "https://", 8) == 0;
}

static void rb_radio_e2e_print_escaped_preview(const char *text, int max_chars)
{
    int i;

    if (!text || max_chars <= 0) {
        printf("(none)");
        return;
    }

    for (i = 0; text[i] && i < max_chars; i++) {
        unsigned char ch;

        ch = (unsigned char)text[i];
        if (ch == '\n') {
            printf("\\n");
        } else if (ch == '\r') {
            printf("\\r");
        } else if (ch == '\t') {
            printf("\\t");
        } else if (ch == '\\') {
            printf("\\\\");
        } else if (ch == '"') {
            printf("\\\"");
        } else if (ch < 32 || ch >= 127) {
            printf("\\x%02X", (unsigned int)ch);
        } else {
            putchar((int)ch);
        }
    }
}

int main(void)
{
    static char path[512];
    static char body[RB_RADIO_E2E_BODY_SIZE];
    static RadioBrowserStation stations[RB_MAX_STATIONS];
    unsigned char peek[RB_RADIO_E2E_PEEK_SIZE];
    RbStreamInfo info;
    char display[RB_MAX_NAME];
    const char *play_url;
    int rc;
    int count;
    int i;
    int peek_len;

    rc = rb_build_station_search_path(path, (int)sizeof(path), "groove", 0, "MP3", 0, -1, 10, 0);
    if (rc < 0) {
        printf("build search path failed: %d\n", rc);
        return 1;
    }
    RADIO_DBG(printf("search path: %s\n", path);)

    rc = rb_http_get_json(RB_RADIO_E2E_HOST, path, body, (int)sizeof(body));
    RADIO_DBG(printf("rb_http_get_json return: %d\n", rc);)
    if (rc < 0) {
        RADIO_DBG(printf("rb_http_get_json error: %d\n", rc);)
        return 1;
    }
    RADIO_DBG(printf("JSON body length: %lu\n", (unsigned long)strlen(body));)
    RADIO_DBG(printf("JSON body first %d chars: \"", RB_RADIO_E2E_BODY_PREVIEW);)
    rb_radio_e2e_print_escaped_preview(body, RB_RADIO_E2E_BODY_PREVIEW);
    printf("\"\n");

    count = rb_parse_stations_json(body, stations, RB_MAX_STATIONS);
    RADIO_DBG(printf("rb_parse_stations_json return/count: %d\n", count);)
    if (count == 0) {
        RADIO_DBG(printf("station count is 0; not probing streams\n");)
        return 1;
    }
    if (count < 0) {
        RADIO_DBG(printf("rb_parse_stations_json error: %d\n", count);)
        return 1;
    }

    for (i = 0; i < count; i++) {
        play_url = rb_station_play_url(&stations[i]);
        if (!play_url[0]) continue;
        if (rb_radio_e2e_is_https_url(play_url)) {
            RADIO_DBG(printf("skip https station %d: %s\n", i + 1, play_url);)
            continue;
        }
        if (!rb_radio_e2e_is_http_url(play_url)) {
            RADIO_DBG(printf("skip non-http station %d: %s\n", i + 1, play_url);)
            continue;
        }

        peek_len = 0;
        rc = rb_probe_stream_url(play_url, &info, peek, (int)sizeof(peek), &peek_len);
        if (rc == RB_STREAM_PROBE_ERR_UNSUPPORTED_TLS) {
            RADIO_DBG(printf("skip station %d: redirect to unsupported TLS\n", i + 1);)
            continue;
        }
        if (rc < 0) {
            RADIO_DBG(printf("skip station %d: probe error %d\n", i + 1, rc);)
            continue;
        }
        if (info.codec != RB_STREAM_CODEC_MP3 && info.codec != RB_STREAM_CODEC_AAC) {
            RADIO_DBG(printf("skip station %d: unsupported/unknown codec %s\n",
                   i + 1, rb_radio_e2e_codec_name(info.codec));)
            continue;
        }

        rb_station_display_name(&stations[i], display, (int)sizeof(display));
        RADIO_DBG(printf("selected station display name: %s\n", display);)
        printf("radio browser codec: %s\n", stations[i].codec);
        printf("radio browser bitrate: %d\n", stations[i].bitrate);
        printf("radio browser country: %s\n", stations[i].countrycode);
        RADIO_DBG(printf("original play URL: %s\n", play_url);)
        RADIO_DBG(printf("final URL: %s\n", info.final_url);)
        RADIO_DBG(printf("redirect count: %d\n", info.redirect_count);)
        RADIO_DBG(printf("HTTP status: %d\n", info.http_status);)
        RADIO_DBG(printf("content type: %s\n", info.content_type);)
        RADIO_DBG(printf("icy-name: %s\n", info.icy_name);)
        RADIO_DBG(printf("icy-br: %d\n", info.icy_br);)
        RADIO_DBG(printf("icy-metaint: %d\n", info.icy_metaint);)
        RADIO_DBG(printf("detected codec: %s\n", rb_radio_e2e_codec_name(info.codec));)
        RADIO_DBG(printf("peek byte count: %d\n", peek_len);)
        return 0;
    }

    RADIO_DBG(printf("no MP3 or AAC station probed successfully\n");)
    return 1;
}
#endif
