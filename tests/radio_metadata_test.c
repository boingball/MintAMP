#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../radio_metadata.h"

static void expect_title(const char *xml, const char *expected)
{
    char out[128];
    int result = RadioMetadata_ExtractXmlTitle(
        out, sizeof(out), (const unsigned char *)xml, strlen(xml));
    assert(result == RADIO_METADATA_TITLE);
    assert(strcmp(out, expected) == 0);
}

int main(void)
{
    char out[32];
    char block[RADIO_METADATA_ICY_MAX];
    unsigned char malformed[RADIO_METADATA_ICY_MAX];
    const char *m80 =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<RadioInfo><Table>"
        "<DB_ALBUM_NAME>Millenium</DB_ALBUM_NAME>"
        "<DB_DALET_ARTIST_NAME>Fallback Artist</DB_DALET_ARTIST_NAME>"
        "<DB_DALET_TITLE_NAME>Fallback Title</DB_DALET_TITLE_NAME>"
        "<DB_LEAD_ARTIST_NAME>Backstreet Boys</DB_LEAD_ARTIST_NAME>"
        "<DB_ALBUM_IMAGE>00000002235.jpg</DB_ALBUM_IMAGE>"
        "<DB_UNUSED>01234567890123456789012345678901234567890123456789"
        "01234567890123456789012345678901234567890123456789"
        "01234567890123456789012345678901234567890123456789"
        "01234567890123456789012345678901234567890123456789"
        "01234567890123456789012345678901234567890123456789"
        "01234567890123456789012345678901234567890123456789"
        "</DB_UNUSED>"
        "<DB_SONG_NAME>I Want It that Way</DB_SONG_NAME>"
        "</Table></RadioInfo>";
    const char *fallback =
        "<RadioInfo><Table>"
        "<DB_DALET_ARTIST_NAME>Hall &amp; Oates</DB_DALET_ARTIST_NAME>"
        "<DB_DALET_TITLE_NAME>Don&apos;t Stop</DB_DALET_TITLE_NAME>"
        "</Table></RadioInfo>";
    const char *unknown =
        "<?xml version=\"1.0\"?><RadioInfo><Table>"
        "<DB_ALBUM_NAME>Album only</DB_ALBUM_NAME>"
        "</Table></RadioInfo>";
    const char *plain = "Ordinary Artist - Ordinary Title";
    int result;

    assert(strlen(m80) > 512);
    assert(strlen(m80) < RADIO_METADATA_ICY_MAX);
    expect_title(m80, "Backstreet Boys - I Want It that Way");
    expect_title(fallback, "Hall & Oates - Don't Stop");

    snprintf(block, sizeof(block), "StreamTitle='%s';StreamUrl='';", m80);
    result = RadioMetadata_ExtractIcyTitle(
        out, sizeof(out), (const unsigned char *)block, strlen(block));
    assert(result == RADIO_METADATA_TITLE);
    assert(strcmp(out, "Backstreet Boys - I Want It tha") == 0);

    strcpy(block, "StreamTitle='Guns N' Roses - Paradise City';StreamUrl='';");
    result = RadioMetadata_ExtractIcyTitle(
        out, sizeof(out), (const unsigned char *)block, strlen(block));
    assert(result == RADIO_METADATA_TITLE);
    assert(strcmp(out, "Guns N' Roses - Paradise City") == 0);

    strcpy(out, "not-cleared");
    result = RadioMetadata_ExtractXmlTitle(
        out, sizeof(out), (const unsigned char *)unknown, strlen(unknown));
    assert(result == RADIO_METADATA_XML_IGNORED);
    assert(out[0] == 0);

    strcpy(out, "not-cleared");
    result = RadioMetadata_ExtractXmlTitle(
        out, sizeof(out), (const unsigned char *)plain, strlen(plain));
    assert(result == RADIO_METADATA_NOT_FOUND);
    assert(out[0] == 0);

    result = RadioMetadata_ExtractXmlTitle(
        out, sizeof(out), (const unsigned char *)m80, strlen(m80));
    assert(result == RADIO_METADATA_TITLE);
    assert(out[sizeof(out) - 1] == 0);
    assert(strlen(out) < sizeof(out));

    memset(malformed, 'X', sizeof(malformed));
    memcpy(malformed, "StreamTitle='<?xml", 18);
    result = RadioMetadata_ExtractIcyTitle(
        out, sizeof(out), malformed, sizeof(malformed));
    assert(result == RADIO_METADATA_XML_IGNORED);
    assert(out[0] == 0);

    puts("RadioInfo metadata tests passed");
    return 0;
}
