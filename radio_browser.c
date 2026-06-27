/*
 * Non-GUI Radio Browser search wrapper.
 *
 * Test harness build:
 *   gcc -std=gnu89 -Wall -Wextra -DRB_SEARCH_API_TEST radio_browser.c radio_browser_url.c radio_browser_http.c radio_browser_json.c -o /tmp/rb_search_api_test
 */

#include "radio_browser.h"
#include "radio_browser_http.h"
#include "radio_browser_url.h"

#define RB_SEARCH_PATH_SIZE 512

int rb_search_stations(
    const char *host,
    const char *name,
    const char *tag,
    const char *codec,
    const char *countrycode,
    int max_bitrate,
    int limit,
    int offset,
    RadioBrowserStation *stations,
    int max_stations,
    char *json_buffer,
    int json_buffer_size
)
{
    char path[RB_SEARCH_PATH_SIZE];
    int rc;

    rc = rb_build_station_search_path(path, (int)sizeof(path), name, tag, codec,
                                      countrycode, max_bitrate, limit, offset);
    if (rc < 0) return rc;

    rc = rb_http_get_json(host, path, json_buffer, json_buffer_size);
    if (rc < 0) return rc;

    return rb_parse_stations_json(json_buffer, stations, max_stations);
}

#ifdef RB_SEARCH_API_TEST
#include <stdio.h>
#define RB_SEARCH_API_HOST "de1.api.radio-browser.info"
#define RB_SEARCH_API_JSON_SIZE 65536
#define RB_SEARCH_API_MAX_STATIONS 10

int main(void)
{
    static RadioBrowserStation stations[RB_SEARCH_API_MAX_STATIONS];
    static char json_buffer[RB_SEARCH_API_JSON_SIZE];
    char display[RB_MAX_NAME];
    int count;
    int i;

    count = rb_search_stations(RB_SEARCH_API_HOST,
                               "groove",
                               (const char *)0,
                               "MP3",
                               (const char *)0,
                               -1,
                               10,
                               0,
                               stations,
                               RB_SEARCH_API_MAX_STATIONS,
                               json_buffer,
                               (int)sizeof(json_buffer));
    if (count < 0) {
        printf("rb_search_stations error: %d\n", count);
        return 1;
    }

    printf("station count: %d\n", count);
    for (i = 0; i < count; i++) {
        rb_station_display_name(&stations[i], display, (int)sizeof(display));
        printf("%d: %s | codec=%s | bitrate=%d | country=%s | url=%s\n",
               i + 1,
               display,
               stations[i].codec,
               stations[i].bitrate,
               stations[i].countrycode,
               rb_station_play_url(&stations[i]));
    }

    return 0;
}
#endif
