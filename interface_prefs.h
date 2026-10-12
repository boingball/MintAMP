#ifndef MINTAMP_INTERFACE_PREFS_H
#define MINTAMP_INTERFACE_PREFS_H
#include "launch_options.h"
struct Window;
void mint_interface_load(MintLaunchOptions *o);
void mint_interface_tooltypes(MintLaunchOptions *o,int argc,char **argv);
/* Shared native requester; saves choices for MintAMP-Launch's next launch. */
void mint_interface_preferences(struct Window *parent);
void mint_interface_flush(void);
void mint_interface_flush_skin(void);
#endif
