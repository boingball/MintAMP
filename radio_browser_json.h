#ifndef RADIO_BROWSER_JSON_H
#define RADIO_BROWSER_JSON_H

#define RB_MAX_STATIONS   50
#define RB_UUID_LEN       37
#define RB_MAX_NAME       256
#define RB_MAX_URL        512
#define RB_MAX_FAVICON    512
#define RB_MAX_TAGS       256
#define RB_MAX_CODEC      16
#define RB_MAX_COUNTRY    4

typedef struct RadioBrowserStation {
    char stationuuid[RB_UUID_LEN];
    char name[RB_MAX_NAME];
    char url[RB_MAX_URL];
    char url_resolved[RB_MAX_URL];
    char favicon[RB_MAX_FAVICON];
    char tags[RB_MAX_TAGS];
    char countrycode[RB_MAX_COUNTRY];
    char codec[RB_MAX_CODEC];
    int bitrate;
    int lastcheckok;
    int hls;
} RadioBrowserStation;

int rb_parse_stations_json(
    const char *json,
    RadioBrowserStation *stations,
    int max_stations
);

const char *rb_station_play_url(
    const RadioBrowserStation *station
);

void rb_station_display_name(
    const RadioBrowserStation *station,
    char *out,
    int out_size
);

#endif
