/* Host test for mint_args_split(): AmigaDOS Shell argument rules. */
#include <stdio.h>
#include <string.h>
#include "mint_args.h"

static int failures;
static void expect(const char *line, int count, const char *const *want)
{
    char text[256], *words[8];
    int n = mint_args_split(line, text, sizeof(text), words, 8), i;
    if (n != count) { printf("FAIL %s: %d words, want %d\n", line, n, count); ++failures; return; }
    for (i = 0; i < n; ++i)
        if (strcmp(words[i], want[i])) { printf("FAIL %s: word %d '%s' want '%s'\n", line, i, words[i], want[i]); ++failures; }
}

int main(void)
{
    static const char *const a[] = {"Music:track.mp3", "--skin", "Work:Skins/a b.wsz", "--double"};
    static const char *const b[] = {"say \"hi\"", "*", "x\ny"};
    static const char *const c[] = {"GUI=REACTION", "MODE=SKINNED", "DOUBLESIZE=YES"};
    static const char *const d[] = {"", "tail"};
    char small[6], *w[4];
    expect("Music:track.mp3 --skin \"Work:Skins/a b.wsz\" --double\n", 4, a);
    expect("  \"say *\"hi*\"\" * \"x*Ny\"", 3, b);
    expect("GUI=REACTION\tMODE=SKINNED DOUBLESIZE=YES\n", 3, c);
    expect("\"\" tail", 2, d);
    expect("", 0, NULL);
    expect("   \n", 0, NULL);
    /* Bounded: never writes past the buffer or the word table. */
    if (mint_args_split("abcdefgh ijk", small, sizeof(small), w, 4) != 1 || strcmp(w[0], "abcde")) { puts("FAIL bounded text"); ++failures; }
    if (mint_args_split("a b c d e f", small, sizeof(small), w, 2) != 2) { puts("FAIL bounded words"); ++failures; }
    if (!failures) puts("mint_args_test: all passed");
    return failures != 0;
}
