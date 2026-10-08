#ifndef RADIO_FAVOURITES_H
#define RADIO_FAVOURITES_H

#include <stddef.h>
#include <string.h>

#include "radio_browser_json.h"

/* The radio favourites are saved as one ENV:/ENVARC: variable rather than a
 * name and a URL variable per slot: every ENVARC: variable is a file on disk,
 * and rewriting 100 of them after each edit froze the GUI for up to a minute.
 *
 * Text form: a header line, then a name line and a URL line per favourite.
 *
 *   MintAMP favourites 1
 *   Jazz FM
 *   http://example.com/jazz
 *
 * Line breaks in a name become spaces; URLs never contain them. */

#define RADIO_FAV_HEADER "MintAMP favourites 1\n"

/* Bytes needed to save `count' favourites (including the NUL). */
#define RADIO_FAV_TEXT_MAX(count) \
    (sizeof(RADIO_FAV_HEADER) + (size_t)(count) * (RB_MAX_NAME + RB_MAX_URL + 2))

/* Writes the text form into out; returns its length, or 0 if out is too
 * small (out is then the empty string). */
static size_t radio_fav_format(char *out, size_t cap, char (*names)[RB_MAX_NAME],
                               char (*urls)[RB_MAX_URL], int count)
{
    size_t used = 0;
    int i;

    if (!out || cap == 0)
        return 0;
    out[0] = '\0';
    if (cap <= sizeof(RADIO_FAV_HEADER) - 1)
        return 0;
    memcpy(out, RADIO_FAV_HEADER, sizeof(RADIO_FAV_HEADER) - 1);
    used = sizeof(RADIO_FAV_HEADER) - 1;
    for (i = 0; i < count; i++) {
        size_t nl = strlen(names[i]);
        size_t ul = strlen(urls[i]);
        size_t k;
        if (nl > RB_MAX_NAME - 1) nl = RB_MAX_NAME - 1;
        if (ul > RB_MAX_URL - 1) ul = RB_MAX_URL - 1;
        if (used + nl + ul + 2 >= cap) {
            out[0] = '\0';
            return 0;
        }
        for (k = 0; k < nl; k++) {
            char c = names[i][k];
            out[used++] = (c == '\n' || c == '\r') ? ' ' : c;
        }
        out[used++] = '\n';
        memcpy(out + used, urls[i], ul);
        used += ul;
        out[used++] = '\n';
    }
    out[used] = '\0';
    return used;
}

/* Copies one line of text[*pos..len) into dst (truncated to fit); returns 0
 * at the end of the text. */
static int radio_fav_line(const char *text, size_t len, size_t *pos, char *dst, size_t dstSize)
{
    size_t start = *pos, end = start, n;

    if (start >= len)
        return 0;
    while (end < len && text[end] != '\n')
        end++;
    *pos = end < len ? end + 1 : end;
    n = end - start;
    if (n > 0 && text[start + n - 1] == '\r')
        n--;
    if (n >= dstSize)
        n = dstSize - 1;
    memcpy(dst, text + start, n);
    dst[n] = '\0';
    return 1;
}

/* Reads the text form; returns the number of favourites, or -1 if text is
 * not in this form. Entries without a URL are dropped. */
static int radio_fav_parse(const char *text, size_t len, char (*names)[RB_MAX_NAME],
                           char (*urls)[RB_MAX_URL], int max)
{
    char header[sizeof(RADIO_FAV_HEADER)];
    size_t pos = 0;
    int count = 0;

    if (!text || !radio_fav_line(text, len, &pos, header, sizeof(header)) ||
        strlen(header) != sizeof(RADIO_FAV_HEADER) - 2 ||
        memcmp(header, RADIO_FAV_HEADER, sizeof(RADIO_FAV_HEADER) - 2) != 0)
        return -1;
    while (count < max &&
           radio_fav_line(text, len, &pos, names[count], RB_MAX_NAME) &&
           radio_fav_line(text, len, &pos, urls[count], RB_MAX_URL)) {
        if (urls[count][0])
            count++;
    }
    return count;
}

#endif
