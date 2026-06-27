/*
 * Test harness build:
 *   gcc -std=gnu89 -Wall -Wextra -DRB_URL_TEST radio_browser_url.c -o /tmp/rb_url_test
 */

#include "radio_browser_url.h"

#include <stdio.h>
#include <string.h>

#define RB_STATION_SEARCH_PATH "/json/stations/search"

static int rb_is_unreserved(unsigned char ch)
{
    if (ch >= 'A' && ch <= 'Z') return 1;
    if (ch >= 'a' && ch <= 'z') return 1;
    if (ch >= '0' && ch <= '9') return 1;
    return ch == '-' || ch == '_' || ch == '.' || ch == '~';
}

static int rb_append_char(char *out, int out_size, int *pos, char ch)
{
    if (!out || !pos || out_size <= 0) return -2;
    if (*pos < 0) return -2;
    if (*pos >= out_size - 1) {
        out[out_size - 1] = '\0';
        return -1;
    }
    out[*pos] = ch;
    (*pos)++;
    out[*pos] = '\0';
    return 0;
}

static int rb_append_str(char *out, int out_size, int *pos, const char *text)
{
    int rc;

    if (!text) return 0;
    while (*text) {
        rc = rb_append_char(out, out_size, pos, *text);
        if (rc < 0) return rc;
        text++;
    }
    return 0;
}

static int rb_append_uint(char *out, int out_size, int *pos, int value)
{
    char buf[16];

    if (value < 0) return 0;
    sprintf(buf, "%d", value);
    return rb_append_str(out, out_size, pos, buf);
}

static int rb_append_hex_escape(char *out, int out_size, int *pos, unsigned char ch)
{
    static const char hex[] = "0123456789ABCDEF";
    int rc;

    rc = rb_append_char(out, out_size, pos, '%');
    if (rc < 0) return rc;
    rc = rb_append_char(out, out_size, pos, hex[(ch >> 4) & 15]);
    if (rc < 0) return rc;
    return rb_append_char(out, out_size, pos, hex[ch & 15]);
}

static int rb_append_encoded(char *out, int out_size, int *pos, const char *text)
{
    int rc;

    if (!text) return 0;
    while (*text) {
        unsigned char ch;

        ch = (unsigned char)*text;
        if (ch == ' ') {
            rc = rb_append_char(out, out_size, pos, '+');
        } else if (rb_is_unreserved(ch)) {
            rc = rb_append_char(out, out_size, pos, (char)ch);
        } else {
            rc = rb_append_hex_escape(out, out_size, pos, ch);
        }
        if (rc < 0) return rc;
        text++;
    }
    return 0;
}

static int rb_append_param_prefix(char *out, int out_size, int *pos, int *count)
{
    int rc;

    rc = rb_append_char(out, out_size, pos, *count == 0 ? '?' : '&');
    if (rc < 0) return rc;
    (*count)++;
    return 0;
}

static int rb_append_param_str(char *out, int out_size, int *pos, int *count,
                               const char *key, const char *value)
{
    int rc;

    if (!value || !*value) return 0;
    rc = rb_append_param_prefix(out, out_size, pos, count);
    if (rc < 0) return rc;
    rc = rb_append_str(out, out_size, pos, key);
    if (rc < 0) return rc;
    rc = rb_append_char(out, out_size, pos, '=');
    if (rc < 0) return rc;
    return rb_append_encoded(out, out_size, pos, value);
}

static int rb_append_param_int(char *out, int out_size, int *pos, int *count,
                               const char *key, int value)
{
    int rc;

    if (value < 0) return 0;
    rc = rb_append_param_prefix(out, out_size, pos, count);
    if (rc < 0) return rc;
    rc = rb_append_str(out, out_size, pos, key);
    if (rc < 0) return rc;
    rc = rb_append_char(out, out_size, pos, '=');
    if (rc < 0) return rc;
    return rb_append_uint(out, out_size, pos, value);
}

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
)
{
    int pos;
    int count;
    int rc;

    if (!out || out_size <= 0) return -2;
    out[0] = '\0';
    pos = 0;
    count = 0;

    rc = rb_append_str(out, out_size, &pos, RB_STATION_SEARCH_PATH);
    if (rc < 0) return rc;

    rc = rb_append_param_str(out, out_size, &pos, &count, "name", name);
    if (rc < 0) return rc;
    rc = rb_append_param_str(out, out_size, &pos, &count, "tag", tag);
    if (rc < 0) return rc;
    rc = rb_append_param_str(out, out_size, &pos, &count, "codec", codec);
    if (rc < 0) return rc;
    rc = rb_append_param_str(out, out_size, &pos, &count, "countrycode", countrycode);
    if (rc < 0) return rc;
    if (max_bitrate > 0) {
        rc = rb_append_param_int(out, out_size, &pos, &count, "bitrateMax", max_bitrate);
        if (rc < 0) return rc;
    }
    rc = rb_append_param_int(out, out_size, &pos, &count, "limit", limit);
    if (rc < 0) return rc;
    rc = rb_append_param_int(out, out_size, &pos, &count, "offset", offset);
    if (rc < 0) return rc;

    rc = rb_append_param_str(out, out_size, &pos, &count, "hidebroken", "true");
    if (rc < 0) return rc;
    rc = rb_append_param_str(out, out_size, &pos, &count, "order", "clickcount");
    if (rc < 0) return rc;
    return rb_append_param_str(out, out_size, &pos, &count, "reverse", "true");
}

#ifdef RB_URL_TEST
static int rb_url_contains(const char *s, const char *needle)
{
    int n;

    if (!s || !needle || !*needle) return 0;
    n = (int)strlen(needle);
    while (*s) {
        if (strncmp(s, needle, (size_t)n) == 0) return 1;
        s++;
    }
    return 0;
}

static int rb_url_test_case(const char *label, const char *name, const char *tag,
                            const char *codec, const char *countrycode,
                            int max_bitrate, const char *must_have,
                            const char *must_not_have)
{
    char path[512];
    int rc;

    rc = rb_build_station_search_path(path, (int)sizeof(path), name, tag, codec,
                                      countrycode, max_bitrate, 25, 0);
    printf("%s: rc=%d %s\n", label, rc, path);
    if (rc < 0) return 1;
    if (must_have && !rb_url_contains(path, must_have)) {
        printf("FAIL: missing %s\n", must_have);
        return 1;
    }
    if (must_not_have && rb_url_contains(path, must_not_have)) {
        printf("FAIL: unexpectedly found %s\n", must_not_have);
        return 1;
    }
    return 0;
}

int main(void)
{
    int fails = 0;

    fails += rb_url_test_case("name=groove codec=MP3", "groove", 0, "MP3", 0, -1, 0, "bitrateMax");
    fails += rb_url_test_case("name=chill codec=AAC+", "chill", 0, "AAC+", 0, -1, 0, "bitrateMax");
    fails += rb_url_test_case("tag=deep house", 0, "deep house", 0, 0, -1, 0, "bitrateMax");
    fails += rb_url_test_case("countrycode=GB codec=AAC", 0, 0, "AAC", "GB", -1, 0, "bitrateMax");
    fails += rb_url_test_case("symbols", "rock & roll + dance", 0, 0, 0, -1, 0, "bitrateMax");
    fails += rb_url_test_case("name=bbc max=Any", "bbc", 0, "MP3", 0, -1, 0, "bitrateMax=0");
    fails += rb_url_test_case("name=bbc max=56", "bbc", 0, "MP3", 0, 56, "bitrateMax=56", 0);
    return fails ? 1 : 0;
}
#endif
