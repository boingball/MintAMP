#ifndef RADIO_STREAM_PROBE_H
#define RADIO_STREAM_PROBE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RB_STREAM_CODEC_UNKNOWN = 0,
    RB_STREAM_CODEC_MP3,
    RB_STREAM_CODEC_AAC
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
    RB_STREAM_PROBE_ERR_TLS_HANDSHAKE = -15
};

const char *rb_probe_error_text(int rc);
int rb_probe_url_looks_hls(const char *url);

int rb_probe_stream_url(
    const char *url,
    RbStreamInfo *info,
    unsigned char *peek_buf,
    int peek_buf_size,
    int *peek_len
);

#ifdef __cplusplus
}
#endif

#endif
