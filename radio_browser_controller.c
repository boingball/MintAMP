/*
 * Small app-facing Radio Browser controller layer.
 *
 * Test harness build:
 *   gcc -std=gnu89 -Wall -Wextra -DRB_CONTROLLER_TEST radio_browser_controller.c radio_browser.c radio_browser_url.c radio_browser_http.c radio_browser_json.c radio_stream_probe.c -o /tmp/rb_controller_test
 */

#include "radio_browser_controller.h"
#include "radio_browser_url.h"

#include <stdio.h>
#include <string.h>

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
}

int rb_controller_search(RadioBrowserController *controller)
{
    int limit;
    int count;

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
    count = rb_search_stations(RB_CONTROLLER_DEFAULT_HOST,
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
    if (count < 0) {
        if (count == -100) {
            rb_controller_set_error(controller, "Response too large; reduce result limit");
            return RB_CONTROLLER_ERR_RESPONSE_TOO_LARGE;
        }
        if (count == -101) {
            rb_controller_set_error(controller, "Parse failed");
            return RB_CONTROLLER_ERR_PARSE_FAILED;
        }
        rb_controller_set_error(controller, "Search failed");
        return RB_CONTROLLER_ERR_SEARCH_FAILED;
    }

    controller->raw_station_count = count;


    controller->station_count = count;
    if (count > 0) controller->selected_index = 0;
    return count;
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

    url = rb_station_play_url(station);
    if (!url || !url[0]) {
        rb_controller_set_error(controller, "Selected station has no stream URL");
        return RB_CONTROLLER_ERR_NO_SELECTION;
    }

#if !defined(HAVE_AMISSL)
    if (rb_controller_starts_with(url, "https://")) {
        rb_controller_set_error(controller, "HTTPS streams are not supported yet");
        return RB_CONTROLLER_ERR_HTTPS_UNSUPPORTED;
    }
#endif

    rc = rb_probe_stream_url(url, info, peek_buf, peek_buf_size, peek_len);
    if (rc < 0) rb_controller_set_error(controller, "Stream probe failed");
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
    rb_test_copy(out, out_size, station ? station->name : "");
}

int rb_probe_stream_url(const char *url, RbStreamInfo *info, unsigned char *peek_buf,
                        int peek_buf_size, int *peek_len)
{
    (void)url; (void)info; (void)peek_buf; (void)peek_buf_size;
    if (peek_len) *peek_len = 0;
    return RB_STREAM_PROBE_OK;
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
