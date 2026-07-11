#ifndef AMIGA_DISPLAY_TEXT_H
#define AMIGA_DISPLAY_TEXT_H

#include <stddef.h>

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
    const char *structuredEnd = NULL;
    unsigned char c;

    if (!dst || dstSize == 0)
        return 0;
    dst[0] = 0;
    if (!src)
        return 0;

    /* Some iHeart/KISS ICY streams put a vendor attribute list inside
     * StreamTitle, for example:
     *
     *   text="Say So" song_spot="M" MediaBaseId="2546146" ...
     *
     * The protocol parser has already bounded and terminated this string, so
     * for display purposes extract only the quoted text value.  Require both
     * the exact leading marker and its closing quote; malformed or ordinary
     * titles continue through the normal conversion path unchanged. */
    if (src[0] == 't' && src[1] == 'e' && src[2] == 'x' &&
        src[3] == 't' && src[4] == '=' && src[5] == '"') {
        const char *end = src + 6;
        while (*end && *end != '"')
            end++;
        if (*end == '"') {
            src += 6;
            structuredEnd = end;
        }
    }

    while (src[si] && (!structuredEnd || src + si < structuredEnd) &&
           di + 1 < dstSize) {
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
