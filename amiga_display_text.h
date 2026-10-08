#ifndef AMIGA_DISPLAY_TEXT_H
#define AMIGA_DISPLAY_TEXT_H

#include <stddef.h>
#include <string.h>

#include "mr_text.h"

/* Convert internet-provided display text to ISO Latin-1 for GUI gadgets and
 * manual text drawing.  This is a display-only helper: do not use it for
 * stream URLs or other protocol data.  The text is read as UTF-8, with bytes
 * that are not valid UTF-8 taken as Windows-1252/Latin-1 (many ICY servers
 * send that).  mr_text.h does the conversion: Latin-1 characters are kept,
 * typographic quotes, dashes and the like become plain ASCII, other accented
 * letters lose the accent, Cyrillic and Greek are transliterated, emoji are
 * dropped and a run of anything else becomes one '?'.  Control characters
 * become spaces, runs of spaces collapse and the result is trimmed. */
static size_t AmigaUtf8ToDisplay(char *dst, size_t dstSize, const char *src)
{
    size_t di;
    char structured[256];
    const char *textMarker;
    const char *titleMarker;
    const char *marker;
    const char *valueStart;
    const char *valueEnd;
    size_t prefixLen;
    size_t valueLen;

    if (!dst || dstSize == 0)
        return 0;
    dst[0] = 0;
    if (!src)
        return 0;

    /* Some iHeart/KISS ICY streams put a vendor attribute list inside
     * StreamTitle.  The station can also prepend the artist using the usual
     * "Artist - Title" convention, producing values such as:
     *
     *   text="Say So" song_spot="M" MediaBaseId="2546146" ...
     *   SZA - title="Snooze",artist="SZA",url="..."
     *   Sombr - text="Back To Friends" song_spot="M" ...
     *
     * Preserve an optional "Artist - " prefix so the GUI's normal title
     * splitter still fills both fields, but replace the vendor payload with
     * only its first quoted text/title value.  Only accept a marker at the
     * start or immediately after " - " to avoid interpreting unrelated text
     * attributes elsewhere in an ordinary title. */
    textMarker = strstr(src, "text=\"");
    titleMarker = strstr(src, "title=\"");
    if (textMarker && titleMarker)
        marker = textMarker < titleMarker ? textMarker : titleMarker;
    else
        marker = textMarker ? textMarker : titleMarker;

    if (marker &&
        (marker == src ||
         (marker >= src + 3 && marker[-3] == ' ' &&
          marker[-2] == '-' && marker[-1] == ' '))) {
        valueStart = marker + (marker == textMarker ? 6 : 7);
        valueEnd = strchr(valueStart, '"');
        if (valueEnd) {
            prefixLen = (size_t)(marker - src);
            valueLen = (size_t)(valueEnd - valueStart);
            if (prefixLen >= sizeof(structured))
                prefixLen = sizeof(structured) - 1;
            if (valueLen > sizeof(structured) - 1 - prefixLen)
                valueLen = sizeof(structured) - 1 - prefixLen;
            if (prefixLen)
                memcpy(structured, src, prefixLen);
            if (valueLen)
                memcpy(structured + prefixLen, valueStart, valueLen);
            structured[prefixLen + valueLen] = 0;
            src = structured;
        }
    }

    di = mr_text_from_utf8(dst, dstSize, src, strlen(src));
    /* A name made only of emoji still names something. */
    if (di == 0 && dstSize > 1) {
        while (*src == ' ' || *src == '\t' || *src == '\n' || *src == '\r')
            src++;
        if (*src) {
            dst[0] = '?';
            dst[1] = 0;
            di = 1;
        }
    }
    return di;
}

#endif /* AMIGA_DISPLAY_TEXT_H */
