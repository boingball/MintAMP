#ifndef RADIO_BROWSER_HTTP_H
#define RADIO_BROWSER_HTTP_H

#define RB_HTTP_ERR_DNS          (-1)
#define RB_HTTP_ERR_CONNECT      (-2)
#define RB_HTTP_ERR_SEND         (-3)
#define RB_HTTP_ERR_READ         (-4)
#define RB_HTTP_ERR_NO_HEADERS   (-5)
#define RB_HTTP_ERR_BODY_TOO_BIG (-6)
#define RB_HTTP_ERR_STATUS       (-7)
#define RB_HTTP_ERR_BAD_ARG      (-8)
#define RB_HTTP_ERR_TIMEOUT      (-9)

int rb_http_get_binary(
    const char *host,
    const char *path,
    unsigned char *out_body,
    int out_body_size
);

int rb_http_get_json(
    const char *host,
    const char *path,
    char *out_body,
    int out_body_size
);

#endif
