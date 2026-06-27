#ifndef RADIO_BROWSER_H
#define RADIO_BROWSER_H

#include "radio_browser_json.h"

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
);

#endif
