#ifndef RADIO_STREAM_PROBE_H
#define RADIO_STREAM_PROBE_H

/* radio_stream.c includes radio_stream.h before this header, while the probe
 * implementation includes this header first.  That lets the persistent
 * network-owner translation unit install the DNS wrapper below without
 * putting bsdsocket calls into radio_stream_probe.c or any other module.
 *
 * A Stop/station-switch signals the persistent network worker with
 * SIGBREAKF_CTRL_C.  If that signal arrives after the previous blocking DNS
 * call has returned, it can remain pending and poison the next station's
 * resolver call.  Clear the stale bit before each DNS lookup and make only
 * that lookup interruptible; remove the break mask immediately afterwards so
 * it cannot affect a later AmiSSL operation on the same worker task. */
#if defined(RADIO_STREAM_H) && defined(AMIGA_M68K) && defined(HAVE_AMISSL)
#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/bsdsocket.h>
#include <netdb.h>

#define RADIO_DNS_JOIN_INNER(a, b) a##b
#define RADIO_DNS_JOIN(a, b) RADIO_DNS_JOIN_INNER(a, b)
#define RADIO_DNS_SOCKET_TAGS RADIO_DNS_JOIN(Socket, BaseTags)

static struct hostent *radio_gethostbyname_scoped(const char *host)
{
    struct hostent *result;

    SetSignal(0, SIGBREAKF_CTRL_C);
    RADIO_DNS_SOCKET_TAGS(SBTM_SETVAL(SBTC_BREAKMASK),
                          (ULONG)SIGBREAKF_CTRL_C,
                          TAG_DONE);
    result = gethostbyname((char *)host);
    RADIO_DNS_SOCKET_TAGS(SBTM_SETVAL(SBTC_BREAKMASK), 0UL, TAG_DONE);
    return result;
}

#define gethostbyname(host) radio_gethostbyname_scoped((host))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RB_STREAM_CODEC_UNKNOWN = 0,
    RB_STREAM_CODEC_MP3,
    RB_STREAM_CODEC_AAC,
    RB_STREAM_CODEC_OGG
} RbStreamCodec;

typedef struct RbStreamInfo {
    int http_status;
    int redirect_count;
    char final_url[512];
    char content_type[64];
    char icy_name[128];
    char icy_url[160];
    int icy_br;
    int icy_metaint;
    RbStreamCodec codec;
    int have_host_addr;
    unsigned long host_addr_be;
} RbStreamInfo;

enum {
    RB_STREAM_PROBE_OK = 0,
    RB_STREAM_PROBE_ERR_BAD_ARG = -1,
    RB_STREAM_PROBE_ERR_UNSUPPORTED_TLS = -2,
    RB_STREAM_PROBE_ERR_BAD_URL = -3,
    RB_STREAM_PROBE_ERR_DNS = -4,
    RB_STREAM_PROBE_ERR_CONNECT = -5,
    RB_STREAM_PROBE_ERR_SEND = -6,
    RB_STREAM_PROBE_ERR_RECV = -7,
    RB_STREAM_PROBE_ERR_HEADERS_TOO_BIG = -8,
    RB_STREAM_PROBE_ERR_REQUEST_TOO_BIG = -9,
    RB_STREAM_PROBE_ERR_TOO_MANY_REDIRECTS = -10,
    RB_STREAM_PROBE_ERR_URL_TOO_LONG = -11,
    RB_STREAM_PROBE_ERR_HLS_UNSUPPORTED = -12,
    RB_STREAM_PROBE_ERR_UNSUPPORTED_CONTENT_TYPE = -13,
    RB_STREAM_PROBE_ERR_SERVER_CLOSED = -14,
    RB_STREAM_PROBE_ERR_TLS_HANDSHAKE = -15,
    RB_STREAM_PROBE_ERR_HTTP_STATUS = -16,
    RB_STREAM_PROBE_ERR_TLS_POISONED = -17,
    /* MiniMem/heap corruption detected earlier this run: every probe/fetch
     * (HTTP and HTTPS alike) is refused until app restart. */
    RB_STREAM_PROBE_ERR_MEM_POISONED = -18,
    /* Runtime optional-network gate disabled the probe/fetch path:
     * MP3_NO_STREAM_PROBE / MP3_NO_ARTWORK, fatal artwork-disable-for-run,
     * or a busy worker/active transport. MP3_TEST_ENABLE_* are accepted as
     * compatibility/debug flags but are no longer required for RC1 defaults. */
    RB_STREAM_PROBE_ERR_DISABLED = -19
};

const char *rb_probe_error_text(int rc);
int rb_probe_url_looks_hls(const char *url);
int rb_probe_stream_probe_test_enabled(void);
int rb_probe_stream_probe_disabled(void);
int rb_probe_artwork_test_enabled(void);
int rb_probe_artwork_disabled(void);
void rb_probe_shutdown_tls_context(void);

int rb_probe_stream_url(
    const char *url,
    RbStreamInfo *info,
    unsigned char *peek_buf,
    int peek_buf_size,
    int *peek_len
);

/* Bounded HTTP/HTTPS GET for non-stream resources (e.g. station favicon
 * images).  Reuses the same connect/TLS/redirect plumbing as
 * rb_probe_stream_url() but applies none of that function's codec/HLS
 * sniffing, so it works for arbitrary content types.  out_buf is filled
 * with up to out_buf_size bytes of response body; *out_len is the number
 * of bytes actually read.  out_content_type (optional) receives the
 * response's Content-Type header, truncated and NUL-terminated. */
int rb_probe_fetch_binary(
    const char *url,
    unsigned char *out_buf,
    int out_buf_size,
    int *out_len,
    char *out_content_type,
    int out_content_type_size
);

#ifdef __cplusplus
}
#endif

#endif
