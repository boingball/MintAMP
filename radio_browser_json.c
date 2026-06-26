/*
 * Test harness builds:
 *   gcc -std=gnu89 -Wall -Wextra -DRB_JSON_TEST radio_browser_json.c -o /tmp/rb_json_test
 *   gcc -std=gnu89 -Wall -Wextra -DRB_JSON_FILE_TEST radio_browser_json.c -o /tmp/rb_json_file_test
 */

#include "radio_browser_json.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char *rb_skip_ws(const char *p)
{
    while (p && *p && isspace((unsigned char)*p)) p++;
    return p;
}

static void rb_station_clear(RadioBrowserStation *station)
{
    if (station) memset(station, 0, sizeof(*station));
}

static int rb_hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static const char *rb_parse_string(const char *p, char *out, int out_size)
{
    int len;

    if (out && out_size > 0) out[0] = '\0';
    if (!p || *p != '"') return p;
    p++;
    len = 0;
    while (*p) {
        char ch;

        if (*p == '"') {
            p++;
            break;
        }
        ch = *p++;
        if (ch == '\\') {
            ch = *p;
            if (!ch) break;
            p++;
            switch (ch) {
            case '"': ch = '"'; break;
            case '\\': ch = '\\'; break;
            case '/': ch = '/'; break;
            case 'n': ch = '\n'; break;
            case 'r': ch = '\r'; break;
            case 't': ch = '\t'; break;
            case 'u': {
                int i;
                for (i = 0; i < 4 && rb_hex_digit(*p) >= 0; i++) p++;
                ch = '?';
                break;
            }
            default:
                break;
            }
        }
        if (out && out_size > 0 && len < out_size - 1) out[len++] = ch;
    }
    if (out && out_size > 0) out[len] = '\0';
    return p;
}

static const char *rb_skip_string(const char *p)
{
    return rb_parse_string(p, (char *)0, 0);
}

static const char *rb_skip_value(const char *p);

static const char *rb_skip_array(const char *p)
{
    p++;
    while (*p) {
        p = rb_skip_ws(p);
        if (*p == ']') return p + 1;
        p = rb_skip_value(p);
        p = rb_skip_ws(p);
        if (*p == ',') p++;
    }
    return p;
}

static const char *rb_skip_object(const char *p)
{
    p++;
    while (*p) {
        p = rb_skip_ws(p);
        if (*p == '}') return p + 1;
        if (*p == '"') p = rb_skip_string(p);
        p = rb_skip_ws(p);
        if (*p == ':') p = rb_skip_value(p + 1);
        p = rb_skip_ws(p);
        if (*p == ',') p++;
    }
    return p;
}

static const char *rb_skip_bare_value(const char *p)
{
    while (*p && *p != ',' && *p != ']' && *p != '}' && !isspace((unsigned char)*p)) p++;
    return p;
}

static const char *rb_skip_value(const char *p)
{
    p = rb_skip_ws(p);
    if (*p == '"') return rb_skip_string(p);
    if (*p == '{') return rb_skip_object(p);
    if (*p == '[') return rb_skip_array(p);
    return rb_skip_bare_value(p);
}

static const char *rb_parse_int_or_bool(const char *p, int *out)
{
    int sign;
    int value;

    p = rb_skip_ws(p);
    if (strncmp(p, "true", 4) == 0) { if (out) *out = 1; return p + 4; }
    if (strncmp(p, "false", 5) == 0) { if (out) *out = 0; return p + 5; }
    sign = 1;
    if (*p == '-') { sign = -1; p++; }
    value = 0;
    while (*p >= '0' && *p <= '9') {
        if (value < 100000000) value = value * 10 + (*p - '0');
        p++;
    }
    if (out) *out = value * sign;
    return p;
}

static int rb_key_equals(const char *key, const char *wanted)
{
    return strcmp(key, wanted) == 0;
}

static const char *rb_parse_station_object(const char *p, RadioBrowserStation *station)
{
    char key[32];

    rb_station_clear(station);
    if (!p || *p != '{') return p;
    p++;
    while (*p) {
        p = rb_skip_ws(p);
        if (*p == '}') return p + 1;
        if (*p != '"') return rb_skip_object(p - 1);
        p = rb_parse_string(p, key, (int)sizeof(key));
        p = rb_skip_ws(p);
        if (*p != ':') return p;
        p++;
        p = rb_skip_ws(p);
        if (rb_key_equals(key, "stationuuid") && *p == '"') p = rb_parse_string(p, station->stationuuid, RB_UUID_LEN);
        else if (rb_key_equals(key, "name") && *p == '"') p = rb_parse_string(p, station->name, RB_MAX_NAME);
        else if (rb_key_equals(key, "url") && *p == '"') p = rb_parse_string(p, station->url, RB_MAX_URL);
        else if (rb_key_equals(key, "url_resolved") && *p == '"') p = rb_parse_string(p, station->url_resolved, RB_MAX_URL);
        else if (rb_key_equals(key, "favicon") && *p == '"') p = rb_parse_string(p, station->favicon, RB_MAX_FAVICON);
        else if (rb_key_equals(key, "tags") && *p == '"') p = rb_parse_string(p, station->tags, RB_MAX_TAGS);
        else if (rb_key_equals(key, "countrycode") && *p == '"') p = rb_parse_string(p, station->countrycode, RB_MAX_COUNTRY);
        else if (rb_key_equals(key, "codec") && *p == '"') p = rb_parse_string(p, station->codec, RB_MAX_CODEC);
        else if (rb_key_equals(key, "bitrate")) p = rb_parse_int_or_bool(p, &station->bitrate);
        else if (rb_key_equals(key, "lastcheckok")) p = rb_parse_int_or_bool(p, &station->lastcheckok);
        else if (rb_key_equals(key, "hls")) p = rb_parse_int_or_bool(p, &station->hls);
        else p = rb_skip_value(p);
        p = rb_skip_ws(p);
        if (*p == ',') p++;
    }
    return p;
}

int rb_parse_stations_json(const char *json, RadioBrowserStation *stations, int max_stations)
{
    const char *p;
    int count;
    int limit;

    if (!json || !stations || max_stations <= 0) return 0;
    limit = max_stations;
    if (limit > RB_MAX_STATIONS) limit = RB_MAX_STATIONS;
    p = rb_skip_ws(json);
    if (*p != '[') return 0;
    p++;
    count = 0;
    while (*p && count < limit) {
        p = rb_skip_ws(p);
        if (*p == ']') break;
        if (*p == '{') {
            p = rb_parse_station_object(p, &stations[count]);
            count++;
        } else {
            p = rb_skip_value(p);
        }
        p = rb_skip_ws(p);
        if (*p == ',') p++;
    }
    return count;
}

const char *rb_station_play_url(const RadioBrowserStation *station)
{
    if (!station) return "";
    if (station->url_resolved[0]) return station->url_resolved;
    if (station->url[0]) return station->url;
    return "";
}

void rb_station_display_name(const RadioBrowserStation *station, char *out, int out_size)
{
    const char *p;
    int len;
    int pending_space;

    if (!out || out_size <= 0) return;
    out[0] = '\0';
    if (!station) return;
    p = station->name;
    len = 0;
    pending_space = 0;
    while (*p && len < out_size - 1) {
        unsigned char ch;
        ch = (unsigned char)*p++;
        if (isspace(ch)) {
            if (len > 0) pending_space = 1;
        } else {
            if (pending_space && len < out_size - 1) out[len++] = ' ';
            pending_space = 0;
            if (len < out_size - 1) out[len++] = (char)ch;
        }
    }
    out[len] = '\0';
}

#if defined(RB_JSON_TEST) || defined(RB_JSON_FILE_TEST)
static void rb_print_station(int index, const RadioBrowserStation *station)
{
    char display[80];

    rb_station_display_name(station, display, (int)sizeof(display));
    printf("%d: name=\"%s\" codec=\"%s\" bitrate=%d countrycode=\"%s\" lastcheckok=%d hls=%d url=\"%s\" url_resolved=\"%s\" play_url=\"%s\"\n",
           index + 1, display, station->codec, station->bitrate,
           station->countrycode, station->lastcheckok, station->hls,
           station->url, station->url_resolved, rb_station_play_url(station));
}

static void rb_print_stations(const RadioBrowserStation *stations, int count)
{
    int i;

    printf("station count: %d\n", count);
    for (i = 0; i < count; i++) rb_print_station(i, &stations[i]);
}
#endif

#ifdef RB_JSON_TEST
int main(void)
{
    static const char sample[] =
        "[{\"stationuuid\":\"960cf833-0601-11e8-ae97-52543be04c81\",\"name\":\"SomaFM\\nGroove\\tSalad (128k MP3)\",\"url\":\"https://somafm.com/groovesalad.pls\",\"url_resolved\":\"https://ice5.somafm.com/groovesalad-128-mp3\",\"favicon\":\"https://somafm.com/img3/groovesalad-400.jpg\",\"tags\":\"ambient,chillout\",\"countrycode\":\"US\",\"codec\":\"MP3\",\"bitrate\":128,\"hls\":0,\"lastcheckok\":true},"
        "{\"stationuuid\":\"7e41d6c6-1842-4ecf-9c6e-2901fdca18ca\",\"name\":\"The Jazz Groove - East with a very long display name that keeps going and going so truncation can be tested safely by callers using small buffers\",\"url\":\"http://east-mp3-128.streamthejazzgroove.com/stream\",\"url_resolved\":\"http://east-mp3-128.streamthejazzgroove.com/stream\",\"favicon\":\"\",\"tags\":\"cool jazz,fusion,jazz\",\"countrycode\":\"US\",\"codec\":\"MP3\",\"bitrate\":128,\"hls\":false,\"lastcheckok\":1}]";
    static const char malformed_edge[] =
        "["
        "{\"name\":\"Missing URL Station\",\"url_resolved\":\"\",\"countrycode\":\"GB\",\"codec\":\"AAC\",\"bitrate\":64,\"lastcheckok\":false,\"hls\":true},"
        "{\"name\":\"Escaped URL Station\",\"url\":\"http:\\/\\/example.com\\/stream.mp3\",\"url_resolved\":\"\",\"countrycode\":\"DE\",\"codec\":\"MP3\",\"bitrate\":192,\"lastcheckok\":true,\"hls\":false},"
        "{\"name\":\"Long Station Name 0123456789 0123456789 0123456789 0123456789 0123456789 0123456789 0123456789 0123456789 0123456789 0123456789 0123456789 0123456789 0123456789 0123456789 0123456789 0123456789 0123456789 0123456789 0123456789 0123456789 0123456789 0123456789 0123456789 0123456789 0123456789 END\",\"url\":\"http://long.example/stream\",\"countrycode\":\"US\",\"codec\":\"OGG\",\"bitrate\":96,\"lastcheckok\":true,\"hls\":false}"
        "]";
    RadioBrowserStation stations[RB_MAX_STATIONS];
    int count;

    printf("static sample:\n");
    count = rb_parse_stations_json(sample, stations, RB_MAX_STATIONS);
    rb_print_stations(stations, count);

    printf("empty array:\n");
    count = rb_parse_stations_json("[]", stations, RB_MAX_STATIONS);
    rb_print_stations(stations, count);

    printf("malformed/edge sample:\n");
    count = rb_parse_stations_json(malformed_edge, stations, RB_MAX_STATIONS);
    rb_print_stations(stations, count);
    return 0;
}
#endif

#ifdef RB_JSON_FILE_TEST
#define RB_JSON_FILE_BUFFER_SIZE 65536

int main(int argc, char **argv)
{
    static char json[RB_JSON_FILE_BUFFER_SIZE];
    RadioBrowserStation stations[RB_MAX_STATIONS];
    FILE *fp;
    size_t len;
    int count;

    if (argc < 2) {
        fprintf(stderr, "usage: %s stations.json\n", argv[0]);
        return 1;
    }
    fp = fopen(argv[1], "rb");
    if (!fp) {
        perror(argv[1]);
        return 1;
    }
    len = fread(json, 1, sizeof(json) - 1, fp);
    if (ferror(fp)) {
        perror(argv[1]);
        fclose(fp);
        return 1;
    }
    if (!feof(fp)) {
        fprintf(stderr, "%s: file too large for %lu byte buffer\n", argv[1],
                (unsigned long)sizeof(json));
        fclose(fp);
        return 1;
    }
    fclose(fp);
    json[len] = '\0';

    count = rb_parse_stations_json(json, stations, RB_MAX_STATIONS);
    rb_print_stations(stations, count);
    return 0;
}
#endif
