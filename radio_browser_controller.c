/*
 * Small app-facing Radio Browser controller layer.
 *
 * Test harness build:
 *   gcc -std=gnu89 -Wall -Wextra -DRB_CONTROLLER_TEST radio_browser_controller.c radio_browser.c radio_browser_url.c radio_browser_http.c radio_browser_json.c radio_stream_probe.c -o /tmp/rb_controller_test
 */

#include "radio_browser_controller.h"
#include "amiga_display_text.h"
#include "radio_browser_url.h"
#include "radio_browser_http.h"
#include "radio_debug.h"
#include "radio_runtime_flags.h"

#include <stdio.h>
#include <string.h>

#if defined(AMIGA_M68K)
#include <stdarg.h>
#include <exec/semaphores.h>
#include <proto/exec.h>
/* Same cross-task shared-stdout race as radio_stream.c (see radio_debug.h's
 * RADIO_DBG comment for the field evidence): this controller runs on the
 * GUI task, concurrently with radio_stream.c/amiga_mp3dec.c's own output
 * during station probing/playback. Redirect every printf() in this file
 * through the same shared radio_console_lock. */
extern struct SignalSemaphore radio_console_lock;
static int RadioBrowserControllerLockedPrintf(const char *fmt, ...)
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
#define printf RadioBrowserControllerLockedPrintf
#endif

/* Radio Browser is a mirror network and clients are expected to fail over:
 * a single hardcoded mirror meant one slow/dead mirror turned every search
 * into "Search failed" for the rest of the app's life.  Each search tries
 * the last mirror that worked first, then rotates through the others;
 * whichever answers becomes the preferred mirror for the next search.
 * "all.api.radio-browser.info" is the project's round-robin DNS record and
 * doubles as a catch-all for mirrors this list doesn't know about. */
static const char *rb_controller_hosts[] = {
    RB_CONTROLLER_DEFAULT_HOST,
    "de2.api.radio-browser.info",
    "fi1.api.radio-browser.info",
    "all.api.radio-browser.info"
};
#define RB_CONTROLLER_HOST_COUNT \
    ((int)(sizeof(rb_controller_hosts) / sizeof(rb_controller_hosts[0])))
static int rb_controller_preferred_host = 0;

static int rb_controller_starts_with(const char *s, const char *prefix)
{
    if (!s || !prefix) return 0;
    while (*prefix) {
        if (*s != *prefix) return 0;
        s++;
        prefix++;
    }
    return 1;
}

static RbStreamCodec rb_controller_codec_from_station(const char *codec)
{
    if (!codec) return RB_STREAM_CODEC_UNKNOWN;
    if (rb_controller_starts_with(codec, "MP3") || rb_controller_starts_with(codec, "mp3")) return RB_STREAM_CODEC_MP3;
    if (rb_controller_starts_with(codec, "AAC") || rb_controller_starts_with(codec, "aac")) return RB_STREAM_CODEC_AAC;
    if (rb_controller_starts_with(codec, "OGG") || rb_controller_starts_with(codec, "ogg")) return RB_STREAM_CODEC_OGG;
    return RB_STREAM_CODEC_UNKNOWN;
}

static const char *rb_controller_optional_string(const char *s)
{
    if (!s || !s[0]) return (const char *)0;
    return s;
}

static void rb_controller_set_error(RadioBrowserController *controller,
                                    const char *message)
{
    int len;

    if (!controller) return;
    controller->last_error[0] = '\0';
    if (!message) return;
    len = (int)strlen(message);
    if (len >= RB_CONTROLLER_LAST_ERROR_SIZE)
        len = RB_CONTROLLER_LAST_ERROR_SIZE - 1;
    if (len > 0) memcpy(controller->last_error, message, (size_t)len);
    controller->last_error[len] = '\0';
}

void rb_controller_init(RadioBrowserController *controller)
{
    if (!controller) return;
    memset(controller, 0, sizeof(*controller));
    controller->selected_index = -1;
    controller->limit = 25;
    controller->max_bitrate = -1;
    controller->offset = 0;
    controller->hide_hls = 1;
    controller->hide_offline = 1;
    controller->hide_ssl_error = 1;
}

static int rb_controller_station_allowed(const RadioBrowserController *controller,
                                         const RadioBrowserStation *station)
{
    if (!controller || !station) return 0;
    if (controller->hide_hls && station->hls) return 0;
    if (controller->hide_offline && station->lastcheckok == 0) return 0;
    if (controller->hide_ssl_error && station->ssl_error != 0) return 0;
    return 1;
}

int rb_controller_search(RadioBrowserController *controller)
{
    int limit;
    int count;

    Radio_LogRuntimeFlagsOnce();
    if (!controller) return RB_CONTROLLER_ERR_BAD_ARG;

    controller->station_count = 0;
    controller->raw_station_count = 0;
    controller->selected_index = -1;
    rb_controller_set_error(controller, (const char *)0);

    limit = controller->limit;
    if (limit <= 0 || limit > RB_CONTROLLER_MAX_STATIONS)
        limit = RB_CONTROLLER_MAX_STATIONS;

    rb_build_station_search_path(controller->last_path, RB_CONTROLLER_LAST_PATH_SIZE,
                                 rb_controller_optional_string(controller->name),
                                 rb_controller_optional_string(controller->tag),
                                 rb_controller_optional_string(controller->codec),
                                 rb_controller_optional_string(controller->countrycode),
                                 controller->max_bitrate, limit, controller->offset);
    {
        int attempt;
        count = RB_HTTP_ERR_CONNECT;
        for (attempt = 0; attempt < RB_CONTROLLER_HOST_COUNT; attempt++) {
            int host_index = (rb_controller_preferred_host + attempt) % RB_CONTROLLER_HOST_COUNT;
            const char *host = rb_controller_hosts[host_index];
            count = rb_search_stations(host,
                                       rb_controller_optional_string(controller->name),
                                       rb_controller_optional_string(controller->tag),
                                       rb_controller_optional_string(controller->codec),
                                       rb_controller_optional_string(controller->countrycode),
                                       controller->max_bitrate,
                                       limit,
                                       controller->offset,
                                       controller->stations,
                                       RB_CONTROLLER_MAX_STATIONS,
                                       controller->json_buffer,
                                       RB_CONTROLLER_JSON_BUFFER_SIZE);
            RADIO_DBG(printf("rb-controller: search host=%s attempt=%d/%d rc=%d\n",
                host, attempt + 1, RB_CONTROLLER_HOST_COUNT, count);)
            if (count >= 0) {
                rb_controller_preferred_host = host_index;
                break;
            }
            /* -100 is this client's own response buffer overflowing -- every
             * mirror would answer the same, so don't burn time retrying.
             * Everything else (timeout/DNS/connect/read/status, and -101
             * parse failures from a mirror serving an error page) is worth
             * trying the next mirror for. */
            if (count == -100)
                break;
        }
    }
    if (count < 0) {
        if (count == -100) {
            rb_controller_set_error(controller, "Search result too large");
            return RB_CONTROLLER_ERR_RESPONSE_TOO_LARGE;
        }
        if (count == -101) {
            rb_controller_set_error(controller, "Parse failed");
            return RB_CONTROLLER_ERR_PARSE_FAILED;
        }
        if (count == RB_HTTP_ERR_TIMEOUT) {
            rb_controller_set_error(controller, "Search failed: connection timed out");
        } else if (count == RB_HTTP_ERR_DNS || count == RB_HTTP_ERR_CONNECT ||
                   count == RB_HTTP_ERR_SEND || count == RB_HTTP_ERR_READ) {
            rb_controller_set_error(controller, "Search failed: network unavailable");
        } else {
            rb_controller_set_error(controller, "Search failed");
        }
        return RB_CONTROLLER_ERR_SEARCH_FAILED;
    }

    controller->raw_station_count = count;


    {
        int i;
        int kept;

        kept = 0;
        for (i = 0; i < count; i++) {
            if (!rb_controller_station_allowed(controller, &controller->stations[i]))
                continue;
            if (kept != i)
                controller->stations[kept] = controller->stations[i];
            kept++;
        }
        controller->station_count = kept;
        if (kept > 0) controller->selected_index = 0;
        return kept;
    }
}

const RadioBrowserStation *rb_controller_get_station(
    const RadioBrowserController *controller,
    int index
)
{
    if (!controller) return (const RadioBrowserStation *)0;
    if (index < 0 || index >= controller->station_count)
        return (const RadioBrowserStation *)0;
    return &controller->stations[index];
}

int rb_controller_set_selected(RadioBrowserController *controller, int index)
{
    if (!controller) return RB_CONTROLLER_ERR_BAD_ARG;
    if (index < 0 || index >= controller->station_count) {
        controller->selected_index = -1;
        rb_controller_set_error(controller, "No station selected");
        return RB_CONTROLLER_ERR_NO_SELECTION;
    }
    controller->selected_index = index;
    rb_controller_set_error(controller, (const char *)0);
    return RB_CONTROLLER_OK;
}

int rb_controller_probe_selected(
    RadioBrowserController *controller,
    RbStreamInfo *info,
    unsigned char *peek_buf,
    int peek_buf_size,
    int *peek_len
)
{
    const RadioBrowserStation *station;
    const char *url;
    int rc;

    if (!controller || !info) return RB_CONTROLLER_ERR_BAD_ARG;
    rb_controller_set_error(controller, (const char *)0);

    station = rb_controller_get_station(controller, controller->selected_index);
    if (!station) {
        rb_controller_set_error(controller, "No station selected");
        return RB_CONTROLLER_ERR_NO_SELECTION;
    }

    if (station->hls) {
        rb_controller_set_error(controller, "HLS stream not supported");
        return RB_STREAM_PROBE_ERR_HLS_UNSUPPORTED;
    }
    if (station->lastcheckok == 0) {
        rb_controller_set_error(controller, "Station is offline");
        return RB_CONTROLLER_ERR_NO_SELECTION;
    }
    if (station->ssl_error != 0) {
        rb_controller_set_error(controller, "Station has SSL errors");
        return RB_CONTROLLER_ERR_NO_SELECTION;
    }

    url = rb_station_play_url(station);
    if (!url || !url[0]) {
        rb_controller_set_error(controller, "Selected station has no stream URL");
        return RB_CONTROLLER_ERR_NO_SELECTION;
    }
    if (rb_probe_url_looks_hls(url)) {
        rb_controller_set_error(controller, "HLS stream not supported");
        return RB_STREAM_PROBE_ERR_HLS_UNSUPPORTED;
    }

#if !defined(HAVE_AMISSL)
    if (rb_controller_starts_with(url, "https://")) {
        rb_controller_set_error(controller, "HTTPS not supported in this build");
        return RB_CONTROLLER_ERR_HTTPS_UNSUPPORTED;
    }
#endif

    if (rb_probe_stream_probe_disabled()) {
        memset(info, 0, sizeof(*info));
        snprintf(info->final_url, sizeof(info->final_url), "%s", url);
        info->final_url[sizeof(info->final_url) - 1] = '\0';
        info->codec = rb_controller_codec_from_station(station->codec);
        info->icy_br = station->bitrate;
        if (peek_len) *peek_len = 0;
        printf("radio-probe: stream probe disabled by MP3_NO_STREAM_PROBE, direct playback url=\"%s\"\n", url);
        return RB_STREAM_PROBE_OK;
    }

    rc = rb_probe_stream_url(url, info, peek_buf, peek_buf_size, peek_len);
    if (rc == RB_STREAM_PROBE_ERR_DISABLED) {
        memset(info, 0, sizeof(*info));
        snprintf(info->final_url, sizeof(info->final_url), "%s", url);
        info->final_url[sizeof(info->final_url) - 1] = '\0';
        info->codec = rb_controller_codec_from_station(station->codec);
        info->icy_br = station->bitrate;
        if (peek_len) *peek_len = 0;

        printf("radio-probe: optional probe unavailable; direct playback url=\"%s\"\n", url);
        return RB_STREAM_PROBE_OK;
    }
    if (rc == RB_STREAM_PROBE_ERR_HTTP_STATUS) {
        char msg[RB_CONTROLLER_LAST_ERROR_SIZE];
        snprintf(msg, sizeof(msg), "Stream unavailable (HTTP %d)", info->http_status);
        rb_controller_set_error(controller, msg);
    } else if (rc < 0) {
        rb_controller_set_error(controller, rb_probe_error_text(rc));
    }
    return rc;
}


#ifdef RB_CONTROLLER_TEST
static void rb_test_copy(char *dst, int dst_size, const char *src)
{
    int len;

    if (!dst || dst_size <= 0) return;
    dst[0] = '\0';
    if (!src) return;
    len = (int)strlen(src);
    if (len >= dst_size) len = dst_size - 1;
    if (len > 0) memcpy(dst, src, (size_t)len);
    dst[len] = '\0';
}

const char *rb_station_play_url(const RadioBrowserStation *station)
{
    if (!station) return "";
    if (station->url_resolved[0]) return station->url_resolved;
    return station->url;
}

void rb_station_display_name(const RadioBrowserStation *station, char *out, int out_size)
{
    AmigaUtf8ToDisplay(out, (size_t)(out_size > 0 ? out_size : 0), station ? station->name : "");
}

int rb_probe_stream_url(const char *url, RbStreamInfo *info, unsigned char *peek_buf,
                        int peek_buf_size, int *peek_len)
{
    (void)url; (void)info; (void)peek_buf; (void)peek_buf_size;
    if (peek_len) *peek_len = 0;
    return RB_STREAM_PROBE_OK;
}

const char *rb_probe_error_text(int rc)
{
    (void)rc;
    return "Stream probe failed";
}

int rb_probe_url_looks_hls(const char *url)
{
    return url && strstr(url, ".m3u8") != (char *)0;
}

int rb_probe_stream_probe_disabled(void)
{
    return 0;
}

int rb_search_stations(const char *host, const char *name, const char *tag,
                       const char *codec, const char *countrycode,
                       int max_bitrate, int limit, int offset,
                       RadioBrowserStation *stations, int max_stations,
                       char *json_buffer, int json_buffer_size)
{
    int out = 0;

    (void)host; (void)tag; (void)codec; (void)countrycode;
    (void)limit; (void)offset; (void)json_buffer;
    (void)json_buffer_size;
    if (!stations || max_stations < 4) return -1;
#define RB_TEST_ADD_STATION(st_name, st_url, st_bitrate) \
    do { \
        if (max_bitrate <= 0 || (st_bitrate) <= max_bitrate) { \
            rb_test_copy(stations[out].name, RB_MAX_NAME, (st_name)); \
            rb_test_copy(stations[out].url, RB_MAX_URL, (st_url)); \
            rb_test_copy(stations[out].codec, RB_MAX_CODEC, "MP3"); \
            stations[out].bitrate = (st_bitrate); \
            stations[out].lastcheckok = 1; \
            out++; \
        } \
    } while (0)

    RB_TEST_ADD_STATION(name && name[0] ? name : "BBC Radio 1", "http://example.com/128.mp3", 128);
    RB_TEST_ADD_STATION("BBC Low", "http://example.com/56.mp3", 56);
    RB_TEST_ADD_STATION("The Groove", "http://example.com/64.mp3", 64);
    RB_TEST_ADD_STATION("Groove Unknown", "http://example.com/unknown.mp3", 0);
    return out;
}

static int rb_controller_expect_count(const char *label, const char *name,
                                      int max_bitrate, int expected)
{
    RadioBrowserController controller;
    int rc;

    rb_controller_init(&controller);
    rb_test_copy(controller.name, (int)sizeof(controller.name), name);
    rb_test_copy(controller.codec, (int)sizeof(controller.codec), "MP3");
    controller.limit = 25;
    controller.max_bitrate = max_bitrate;
    rc = rb_controller_search(&controller);
    printf("%s: rc=%d station_count=%d max_bitrate=%d\n", label, rc,
           controller.station_count, controller.max_bitrate);
    if (rc != expected || controller.station_count != expected) {
        printf("FAIL: expected %d\n", expected);
        return 1;
    }
    return 0;
}
#endif

#ifdef RB_CONTROLLER_TEST
int main(void)
{
    int fails = 0;

    fails += rb_controller_expect_count("bbc + Any bitrate", "bbc", -1, 4);
    fails += rb_controller_expect_count("bbc + <=56 API-filtered", "bbc", 56, 2);
    fails += rb_controller_expect_count("groove + Any bitrate", "groove", -1, 4);
    fails += rb_controller_expect_count("groove + <=64 API-filtered", "groove", 64, 3);
    return fails ? 1 : 0;
}
#endif
