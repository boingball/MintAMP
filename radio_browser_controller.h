#ifndef RADIO_BROWSER_CONTROLLER_H
#define RADIO_BROWSER_CONTROLLER_H

#include "radio_browser.h"
#include "radio_stream_probe.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RB_CONTROLLER_MAX_STATIONS 50
#define RB_CONTROLLER_JSON_BUFFER_SIZE 131072
#define RB_CONTROLLER_LAST_ERROR_SIZE 128
#define RB_CONTROLLER_DEFAULT_HOST "de1.api.radio-browser.info"

typedef struct RadioBrowserController {
    RadioBrowserStation stations[RB_CONTROLLER_MAX_STATIONS];
    int station_count;
    int selected_index;
    char json_buffer[RB_CONTROLLER_JSON_BUFFER_SIZE];
    char last_error[RB_CONTROLLER_LAST_ERROR_SIZE];
    char name[RB_MAX_NAME];
    char codec[RB_MAX_CODEC];
    char countrycode[RB_MAX_COUNTRY];
    char tag[RB_MAX_TAGS];
    int limit;
    int offset;
} RadioBrowserController;

enum {
    RB_CONTROLLER_OK = 0,
    RB_CONTROLLER_ERR_BAD_ARG = -1,
    RB_CONTROLLER_ERR_NO_SELECTION = -2,
    RB_CONTROLLER_ERR_HTTPS_UNSUPPORTED = -3
};

void rb_controller_init(RadioBrowserController *controller);
int rb_controller_search(RadioBrowserController *controller);
const RadioBrowserStation *rb_controller_get_station(
    const RadioBrowserController *controller,
    int index
);
int rb_controller_probe_selected(
    RadioBrowserController *controller,
    RbStreamInfo *info,
    unsigned char *peek_buf,
    int peek_buf_size,
    int *peek_len
);

#ifdef __cplusplus
}
#endif

#endif
