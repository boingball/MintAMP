#ifndef MINTAMP_LAUNCH_OPTIONS_H
#define MINTAMP_LAUNCH_OPTIONS_H
#include <stddef.h>
#define MINTAMP_DEFAULT_SKIN "PROGDIR:Skins/MintAMP-mintleaf.wsz"
typedef struct MintLaunchOptions {
    int gadtools, skinned, doublesize, skin_set, size_set;
    char skin[512];
} MintLaunchOptions;
void mint_launch_defaults(MintLaunchOptions *o);
/* Invalid/oversized values leave the preceding layer intact. */
int mint_launch_option(MintLaunchOptions *o,const char *key,const char *value);
const char *mint_launch_binary(const MintLaunchOptions *o);
/* Parse only appearance options; return the sole audio/playlist argument.
 * Unknown switches and missing values are errors, never audio filenames. */
int mint_launch_args(MintLaunchOptions *o,int argc,char **argv,const char **audio);
/* AmigaDOS argument quoting, not shell execution. Reject control characters. */
int mint_launch_quote(char *out,size_t size,const char *arg);
#endif
