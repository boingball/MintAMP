#ifndef RADIO_METADATA_H
#define RADIO_METADATA_H

#include <stddef.h>
#include <string.h>

#define RADIO_METADATA_NOT_FOUND   0
#define RADIO_METADATA_TITLE       1
#define RADIO_METADATA_XML_IGNORED (-1)
#define RADIO_METADATA_ICY_MAX     4096

/* ICY metadata is a counted byte block, not necessarily a C string. Bauer's
 * Portuguese stations place a complete <RadioInfo> document inside
 * StreamTitle. This bounded recogniser extracts only its useful song fields;
 * it is deliberately not a general XML parser. */
static const unsigned char *radio_metadata_find(const unsigned char *src,
                                                 size_t src_len,
                                                 const char *needle)
{
    size_t needle_len, i;
    if (!src || !needle)
        return NULL;
    needle_len = strlen(needle);
    if (needle_len == 0 || needle_len > src_len)
        return NULL;
    for (i = 0; i + needle_len <= src_len; i++) {
        if (memcmp(src + i, needle, needle_len) == 0)
            return src + i;
    }
    return NULL;
}

static int radio_metadata_extract_tag(const unsigned char *src, size_t src_len,
                                      const char *open_tag,
                                      const char *close_tag,
                                      char *dst, size_t dst_size)
{
    const unsigned char *begin, *end, *entity_end;
    size_t open_len, remaining, di, entity_len;
    if (!dst || dst_size == 0)
        return 0;
    dst[0] = 0;
    begin = radio_metadata_find(src, src_len, open_tag);
    if (!begin)
        return 0;
    open_len = strlen(open_tag);
    begin += open_len;
    remaining = src_len - (size_t)(begin - src);
    end = radio_metadata_find(begin, remaining, close_tag);
    if (!end)
        return 0;
    while (begin < end && (*begin == ' ' || *begin == '\t' ||
                           *begin == '\r' || *begin == '\n'))
        begin++;
    while (end > begin && (end[-1] == ' ' || end[-1] == '\t' ||
                            end[-1] == '\r' || end[-1] == '\n'))
        end--;
    di = 0;
    while (begin < end && di + 1 < dst_size) {
        if (*begin != '&') {
            dst[di++] = (char)*begin++;
            continue;
        }
        entity_end = begin;
        while (entity_end < end && entity_end - begin <= 6 &&
               *entity_end != ';')
            entity_end++;
        if (entity_end >= end || *entity_end != ';') {
            dst[di++] = (char)*begin++;
            continue;
        }
        entity_len = (size_t)(entity_end - begin + 1);
        if (entity_len == 5 && memcmp(begin, "&amp;", 5) == 0)
            dst[di++] = '&';
        else if (entity_len == 6 && memcmp(begin, "&apos;", 6) == 0)
            dst[di++] = '\'';
        else if (entity_len == 6 && memcmp(begin, "&quot;", 6) == 0)
            dst[di++] = '"';
        else if (entity_len == 4 && memcmp(begin, "&lt;", 4) == 0)
            dst[di++] = '<';
        else if (entity_len == 4 && memcmp(begin, "&gt;", 4) == 0)
            dst[di++] = '>';
        else {
            dst[di++] = (char)*begin++;
            continue;
        }
        begin = entity_end + 1;
    }
    dst[di] = 0;
    return di != 0;
}

static size_t radio_metadata_append(char *dst, size_t dst_size, size_t used,
                                    const char *src)
{
    if (!dst || dst_size == 0 || !src)
        return used;
    while (*src && used + 1 < dst_size)
        dst[used++] = *src++;
    dst[used] = 0;
    return used;
}

static int RadioMetadata_ExtractXmlTitle(char *dst, size_t dst_size,
                                         const unsigned char *src,
                                         size_t src_len)
{
    const unsigned char *first;
    char artist[128], title[128];
    size_t used;
    int have_artist, have_title;
    if (!dst || dst_size == 0)
        return RADIO_METADATA_NOT_FOUND;
    dst[0] = 0;
    if (!src || src_len == 0)
        return RADIO_METADATA_NOT_FOUND;

    first = src;
    while ((size_t)(first - src) < src_len &&
           (*first == ' ' || *first == '\t' || *first == '\r' ||
            *first == '\n'))
        first++;
    if (!((size_t)(first - src) + 5 <= src_len &&
          memcmp(first, "<?xml", 5) == 0) &&
        !radio_metadata_find(src, src_len, "<RadioInfo"))
        return RADIO_METADATA_NOT_FOUND;

    have_artist = radio_metadata_extract_tag(
        src, src_len, "<DB_LEAD_ARTIST_NAME>", "</DB_LEAD_ARTIST_NAME>",
        artist, sizeof(artist));
    if (!have_artist)
        have_artist = radio_metadata_extract_tag(
            src, src_len, "<DB_DALET_ARTIST_NAME>",
            "</DB_DALET_ARTIST_NAME>", artist, sizeof(artist));
    have_title = radio_metadata_extract_tag(
        src, src_len, "<DB_SONG_NAME>", "</DB_SONG_NAME>", title,
        sizeof(title));
    if (!have_title)
        have_title = radio_metadata_extract_tag(
            src, src_len, "<DB_DALET_TITLE_NAME>",
            "</DB_DALET_TITLE_NAME>", title, sizeof(title));

    if (!have_artist && !have_title)
        return RADIO_METADATA_XML_IGNORED;
    used = 0;
    if (have_artist)
        used = radio_metadata_append(dst, dst_size, used, artist);
    if (have_artist && have_title)
        used = radio_metadata_append(dst, dst_size, used, " - ");
    if (have_title)
        radio_metadata_append(dst, dst_size, used, title);
    return RADIO_METADATA_TITLE;
}

/* Extract one complete StreamTitle value from an ICY metadata block. Looking
 * for the terminating "';" pair, rather than the first apostrophe, also keeps
 * ordinary titles such as "Guns N' Roses" intact. */
static int RadioMetadata_ExtractIcyTitle(char *dst, size_t dst_size,
                                        const unsigned char *src,
                                        size_t src_len)
{
    static const char key[] = "StreamTitle='";
    const unsigned char *begin, *end;
    size_t key_len, remaining, len, i;
    int xml_result;
    if (!dst || dst_size == 0)
        return RADIO_METADATA_NOT_FOUND;
    dst[0] = 0;
    if (!src || src_len == 0)
        return RADIO_METADATA_NOT_FOUND;
    key_len = sizeof(key) - 1;
    begin = radio_metadata_find(src, src_len, key);
    if (!begin)
        return RADIO_METADATA_NOT_FOUND;
    begin += key_len;
    remaining = src_len - (size_t)(begin - src);

    xml_result = RadioMetadata_ExtractXmlTitle(dst, dst_size, begin, remaining);
    if (xml_result == RADIO_METADATA_TITLE)
        return RADIO_METADATA_TITLE;
    if (xml_result == RADIO_METADATA_XML_IGNORED)
        return RADIO_METADATA_XML_IGNORED;

    end = radio_metadata_find(begin, remaining, "';");
    if (!end)
        end = radio_metadata_find(begin, remaining, "'");
    if (!end)
        end = begin + remaining;
    len = (size_t)(end - begin);
    if (len >= dst_size)
        len = dst_size - 1;
    for (i = 0; i < len; i++)
        dst[i] = (char)begin[i];
    dst[len] = 0;
    return RADIO_METADATA_TITLE;
}

#endif /* RADIO_METADATA_H */
