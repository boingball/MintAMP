#ifndef MINTAMP_MINT_ARGS_H
#define MINTAMP_MINT_ARGS_H

/* Give main() the argc/argv every MintAMP entry point expects, whichever C
 * runtime the toolchain linked:
 *   Workbench: argc 0, argv is the struct WBStartup pointer itself.
 *   Shell:     argv[0] program name, argv[1..] AmigaDOS-quoted arguments.
 * The bebbo newlib startup hands main() argc 1 and argv[0] = the raw
 * argument line on the Shell, and an array holding the WBStartup on
 * Workbench, so without this every Shell argument (track, --skin, --double,
 * MintAMP-Launch's hand-off) was lost and ToolTypes were read from the wrong
 * structure. Call it first thing in a real program entry point. */
void mint_args_normalise(int *argc, char ***argv);

/* Host-testable splitter used by mint_args_normalise(): AmigaDOS rules,
 * blanks separate, "..." groups, inside quotes *" *N *E ** are escapes.
 * Writes NUL-terminated words into out (size bytes) and up to max pointers.
 * Returns the number of words. */
int mint_args_split(const char *line, char *out, unsigned size, char **words, int max);

#endif
