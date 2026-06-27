#ifndef RADIO_BROWSER_URL_H
#define RADIO_BROWSER_URL_H

int rb_build_station_search_path(
    char *out,
    int out_size,
    const char *name,
    const char *tag,
    const char *codec,
    const char *countrycode,
    int max_bitrate,
    int limit,
    int offset
);

#endif
