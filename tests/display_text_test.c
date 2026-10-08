#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../amiga_display_text.h"

static void check(const char *in, const char *want)
{
    char out[128];
    AmigaUtf8ToDisplay(out, sizeof(out), in);
    if (strcmp(out, want) != 0) {
        printf("display_text_test: \"%s\" -> \"%s\", want \"%s\"\n", in, out, want);
        assert(0);
    }
}

int main(void)
{
    char small[8];

    /* Station names and ICY titles in UTF-8. */
    check("Classic Hits 70\xe2\x80\x99s \xe2\x80\x93 Disco\xe2\x80\xa6", "Classic Hits 70's - Disco...");
    check("\xe2\x80\x9c" "Caf\xc3\xa9\xe2\x80\x9d Radio", "\"Caf\xe9\" Radio");
    check("Radio Zet \xc5\x81\xc3\xb3" "d\xc5\xba", "Radio Zet L\xf3" "dz");
    check("\xd0\xa0\xd0\xb5\xd1\x82\xd1\x80\xd0\xbe FM", "Retro FM");
    check("\xf0\x9f\x8e\xb5 Disco \xf0\x9f\x95\xba Funk", "Disco Funk");
    check("\xf0\x9f\x8e\xb5\xf0\x9f\x8e\xb6", "?");
    check("NHK \xe3\x83\xa9\xe3\x82\xb8\xe3\x82\xaa", "NHK ?");

    /* ICY servers that send Latin-1 or Windows-1252 bytes. */
    check("Beyonc\xe9 - D\xe9j\xe0 Vu", "Beyonc\xe9 - D\xe9j\xe0 Vu");
    check("Don\x92t Stop \x96 Live", "Don't Stop - Live");

    /* Controls, spacing, and already converted text stays put. */
    check("  A\tB\r\n\x01" "C  ", "A B C");
    check("Caf\xe9 \xb7 Bar", "Caf\xe9 \xb7 Bar");

    /* iHeart/KISS vendor attributes in StreamTitle. */
    check("text=\"Say So\" song_spot=\"M\" MediaBaseId=\"2546146\"", "Say So");
    check("SZA - title=\"Snooze\",artist=\"SZA\",url=\"x\"", "SZA - Snooze");

    /* Never splits a transliteration at the end of the buffer. */
    AmigaUtf8ToDisplay(small, sizeof(small), "Price \xe2\x82\xac" "5");
    assert(!strcmp(small, "Price"));

    printf("display_text_test: ok\n");
    return 0;
}
