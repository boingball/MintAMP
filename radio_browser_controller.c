/*
 * Small app-facing Radio Browser controller layer.
 *
 * Test harness build:
 *   gcc -std=gnu89 -Wall -Wextra -DRB_CONTROLLER_TEST radio_browser_controller.c radio_browser.c radio_browser_url.c radio_browser_http.c radio_browser_json.c radio_stream_probe.c -o /tmp/rb_controller_test
 */

#include "radio_browser_controller.h"

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
    controller->limit = RB_CONTROLLER_MAX_STATIONS;
    controller->offset = 0;
}

int rb_controller_search(RadioBrowserController *controller)
{
    int limit;
    int count;

    if (!controller) return RB_CONTROLLER_ERR_BAD_ARG;

    controller->station_count = 0;
    controller->selected_index = -1;
    rb_controller_set_error(controller, (const char *)0);

    limit = controller->limit;
    if (limit <= 0 || limit > RB_CONTROLLER_MAX_STATIONS)
        limit = RB_CONTROLLER_MAX_STATIONS;

    count = rb_search_stations(RB_CONTROLLER_DEFAULT_HOST,
                               rb_controller_optional_string(controller->name),
                               rb_controller_optional_string(controller->tag),
                               rb_controller_optional_string(controller->codec),
                               rb_controller_optional_string(controller->countrycode),
                               limit,
                               controller->offset,
                               controller->stations,
                               RB_CONTROLLER_MAX_STATIONS,
                               controller->json_buffer,
                               RB_CONTROLLER_JSON_BUFFER_SIZE);
    if (count < 0) {
        rb_controller_set_error(controller, "Radio Browser search failed");
        return count;
    }

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

    if (rb_controller_starts_with(url, "https://")) {
        rb_controller_set_error(controller, "HTTPS streams are not supported yet");
        return RB_CONTROLLER_ERR_HTTPS_UNSUPPORTED;
    }

    rc = rb_probe_stream_url(url, info, peek_buf, peek_buf_size, peek_len);
    if (rc < 0) rb_controller_set_error(controller, "Stream probe failed");
    return rc;
}

#ifdef RB_CONTROLLER_TEST
int main(void)
{
    static RadioBrowserController controller;
    static unsigned char peek_buf[512];
    RbStreamInfo info;
    char display[RB_MAX_NAME];
    int peek_len;
    int rc;
    int i;

    rb_controller_init(&controller);
    strcpy(controller.name, "groove");
    strcpy(controller.codec, "MP3");
    controller.limit = 10;

    rc = rb_controller_search(&controller);
    if (rc < 0) {
        printf("search error: %d (%s)\n", rc, controller.last_error);
        return 1;
    }

    printf("station count: %d\n", controller.station_count);
    controller.selected_index = -1;
    for (i = 0; i < controller.station_count; i++) {
        rb_station_display_name(&controller.stations[i], display, (int)sizeof(display));
        printf("%d: %s | codec=%s | bitrate=%d | country=%s | url=%s\n",
               i + 1,
               display,
               controller.stations[i].codec,
               controller.stations[i].bitrate,
               controller.stations[i].countrycode,
               rb_station_play_url(&controller.stations[i]));
        if (controller.selected_index < 0 &&
            rb_controller_starts_with(rb_station_play_url(&controller.stations[i]), "http://")) {
            controller.selected_index = i;
        }
    }

    if (controller.selected_index < 0) {
        printf("no HTTP station available to probe\n");
        return 0;
    }

    peek_len = 0;
    rc = rb_controller_probe_selected(&controller, &info, peek_buf,
                                      (int)sizeof(peek_buf), &peek_len);
    printf("probe station: %d\n", controller.selected_index + 1);
    printf("probe result: %d (%s)\n", rc, controller.last_error);
    if (rc == RB_STREAM_PROBE_OK) {
        printf("http=%d redirects=%d codec=%d content-type=%s icy-name=%s peek=%d final-url=%s\n",
               info.http_status,
               info.redirect_count,
               info.codec,
               info.content_type,
               info.icy_name,
               peek_len,
               info.final_url);
    }

    return rc < 0 ? 1 : 0;
}
#endif
