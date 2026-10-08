#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../radio_favourites.h"

#define MAX 50

static char names[MAX][RB_MAX_NAME], urls[MAX][RB_MAX_URL];
static char names2[MAX][RB_MAX_NAME], urls2[MAX][RB_MAX_URL];
static char text[RADIO_FAV_TEXT_MAX(MAX)];

int main(void)
{
    size_t n;
    int i, count;

    /* Round trip, including the longest names and URLs. */
    for (i = 0; i < MAX; i++) {
        sprintf(names[i], "Station %d", i);
        sprintf(urls[i], "http://example.com:%d/live", 8000 + i);
    }
    memset(names[3], 'N', RB_MAX_NAME - 1);
    memset(urls[4], 'u', RB_MAX_URL - 1);
    strcpy(names[5], "Two\nLines\r");
    strcpy(names[6], "");
    n = radio_fav_format(text, sizeof(text), names, urls, MAX);
    assert(n > 0 && n == strlen(text));
    assert(!memcmp(text, "MintAMP favourites 1\nStation 0\nhttp://example.com:8000/live\n", 59));
    count = radio_fav_parse(text, n, names2, urls2, MAX);
    assert(count == MAX);
    for (i = 0; i < MAX; i++) {
        if (i == 5) assert(!strcmp(names2[i], "Two Lines "));
        else assert(!strcmp(names2[i], names[i]));
        assert(!strcmp(urls2[i], urls[i]));
    }

    /* No favourites, CRLF line ends, a missing URL, and too many entries. */
    n = radio_fav_format(text, sizeof(text), names, urls, 0);
    assert(n == strlen(RADIO_FAV_HEADER) && radio_fav_parse(text, n, names2, urls2, MAX) == 0);
    strcpy(text, "MintAMP favourites 1\r\nA\r\nhttp://a/\r\nB\r\n\r\nC\nhttp://c/");
    count = radio_fav_parse(text, strlen(text), names2, urls2, MAX);
    assert(count == 2 && !strcmp(names2[1], "C") && !strcmp(urls2[1], "http://c/"));
    n = radio_fav_format(text, sizeof(text), names, urls, MAX);
    assert(radio_fav_parse(text, n, names2, urls2, 7) == 7);

    /* Not this format: the caller falls back to the old variables. */
    assert(radio_fav_parse("Jazz\nhttp://x/\n", 15, names2, urls2, MAX) == -1);
    assert(radio_fav_parse(NULL, 0, names2, urls2, MAX) == -1);

    /* A buffer that is too small gives nothing, not half a list. */
    assert(radio_fav_format(text, 100, names, urls, MAX) == 0 && text[0] == '\0');

    printf("radio_favourites_test: ok\n");
    return 0;
}
