#ifndef AMIGA_DISPLAY_TEXT_H
#define AMIGA_DISPLAY_TEXT_H

#include <stddef.h>
#include <string.h>

/* Convert internet-provided UTF-8 display text to Amiga-safe single-byte
 * Latin-1-ish bytes for GUI gadgets and manual text drawing.  This is a
 * display-only helper: do not use it for stream URLs or other protocol data.
 * ASCII control bytes are removed, with tab/newline/carriage return folded to
 * a normal space so callers that intentionally handle text layout can still
 * collapse whitespace.  Common UTF-8 Latin-1 sequences become their single-byte
 * ISO-8859-1 values; unsupported UTF-8 multibyte runs become '?'. */
static size_t AmigaUtf8ToDisplay(char *dst, size_t dstSize, const char *src)
{
    size_t si = 0, di = 0;
    char structured[256];
    const char *textMarker;
    const char *titleMarker;
    const char *marker;
    const char *valueStart;
    const char *valueEnd;
    size_t prefixLen;
    size_t valueLen;
    unsigned char c;

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

    while (src[si] && di + 1 < dstSize) {
        c = (unsigned char)src[si++];
        if (c == '\t' || c == '\n' || c == '\r') {
            dst[di++] = ' ';
        } else if (c < 0x20 || c == 0x7f) {
            continue;
        } else if (c < 0x80) {
            dst[di++] = (char)c;
        } else if ((c == 0xc2 || c == 0xc3) &&
                   (unsigned char)src[si] >= 0x80 &&
                   (unsigned char)src[si] <= 0xbf) {
            dst[di++] = (char)((c == 0xc2) ? (unsigned char)src[si] :
                ((unsigned char)src[si] + 0x40));
            si++;
        } else if (c >= 0xc0 && src[si] &&
                   (((unsigned char)src[si] & 0xc0) == 0x80)) {
            while (src[si] && (((unsigned char)src[si] & 0xc0) == 0x80))
                si++;
            dst[di++] = '?';
        } else {
            dst[di++] = (c >= 0x80 && c < 0xc0) ? '?' : (char)c;
        }
    }
    dst[di] = 0;
    return di;
}

#endif /* AMIGA_DISPLAY_TEXT_H */
