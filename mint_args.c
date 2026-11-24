/* mint_args.c -- runtime-independent argc/argv for MintAMP entry points.
 * See mint_args.h for why this exists. */
#include "mint_args.h"
#include <string.h>

int mint_args_split(const char *line, char *out, unsigned size, char **words, int max)
{
    unsigned used = 0;
    int count = 0;
    if (!line || !out || !size || !words || max < 1) return 0;
    for (;;) {
        int quoted = 0;
        while (*line == ' ' || *line == '\t' || *line == '\n' || *line == '\r') ++line;
        if (!*line || count >= max || used + 1 >= size) break;
        words[count++] = out + used;
        if (*line == '"') { quoted = 1; ++line; }
        while (*line && used + 1 < size) {
            char c = *line;
            if (quoted) {
                if (c == '"') { ++line; break; }
                if (c == '*' && line[1]) {
                    c = line[1]; line += 2;
                    if (c == 'N' || c == 'n') c = '\n';
                    else if (c == 'E' || c == 'e') c = 27;
                    out[used++] = c;
                    continue;
                }
            } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                break;
            }
            out[used++] = c; ++line;
        }
        out[used++] = 0;
    }
    return count;
}

#if defined(AMIGA_M68K)
#include <exec/types.h>
#include <dos/dos.h>
#include <workbench/startup.h>
#include <proto/dos.h>

/* Both newlib and libnix startups publish the Workbench message here. */
extern struct WBStartup *_WBenchMsg;

#define MINT_ARGS_MAX 32
#define MINT_ARGS_TEXT 1024

void mint_args_normalise(int *argc, char ***argv)
{
    static char name[256], text[MINT_ARGS_TEXT];
    static char *words[MINT_ARGS_MAX + 1];
    STRPTR line;
    int count;
    if (!argc || !argv) return;
    if (_WBenchMsg) {
        *argc = 0;
        *argv = (char **)_WBenchMsg;
        return;
    }
    if (!GetProgramName((STRPTR)name, sizeof(name))) name[0] = 0;
    words[0] = name;
    line = GetArgStr();
    count = 1 + mint_args_split(line ? (const char *)line : "", text, sizeof(text), words + 1, MINT_ARGS_MAX - 1);
    words[count] = NULL;
    *argc = count;
    *argv = words;
}
#else
void mint_args_normalise(int *argc, char ***argv)
{
    (void)argc; (void)argv; /* host C runtimes already split arguments */
}
#endif
