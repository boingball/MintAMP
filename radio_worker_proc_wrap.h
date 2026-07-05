#ifndef RADIO_WORKER_PROC_WRAP_H
#define RADIO_WORKER_PROC_WRAP_H

/* Tactical PR #455 launcher shim.
 *
 * The current frontends start the external radio worker with
 * CreateNewProcTags(NP_Seglist, LoadSeg("amiga_mp3dec.fastexp"),
 * NP_Arguments, ...).  That creates a separate process, but on the m68k NDK/C
 * startup path it does not reliably give the command-line decoder a normal CLI
 * argc/argv, leaving the GUI stuck at "Buffering" with no audio.
 *
 * This header wraps CreateNewProcTags only in the GUI frontend builds.  Normal
 * same-image NP_Entry children are forwarded unchanged.  The specific radio
 * worker NP_Seglist launch is converted into a tiny same-image launcher task;
 * that launcher calls dos.library RunCommand() on the already-loaded external
 * seglist with the exact argument string.  RunCommand gives amiga_mp3dec.fastexp
 * the command-style argv it expects while still keeping the decoder in its own
 * loaded program image/data segment, so bsdsocket.library/AmiSSL ownership stays
 * separated from the GUI.
 *
 * The Makefile currently pre-includes this header for complete GUI link lines
 * that also contain .S assembly sources.  Keep the wrapper completely invisible
 * while the assembler is preprocessing .S files, otherwise C typedefs from
 * stdarg/stddef leak into the assembler input.
 */

#if defined(AMIGA_M68K) && !defined(__ASSEMBLER__)
#include <stdarg.h>
#include <string.h>
#include <exec/types.h>
#include <exec/tasks.h>
#include <utility/tagitem.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>

#ifndef MINIAMP3_PROC_WRAP_MAX_TAGS
#define MINIAMP3_PROC_WRAP_MAX_TAGS 48
#endif

static BPTR miniamp3_radio_worker_seglist = (BPTR)0;
static char miniamp3_radio_worker_args[2300];
static LONG miniamp3_radio_worker_arglen = 0;

static void MiniAmp3RadioWorkerLauncherEntry(void)
{
    BPTR seg;
    char *args;
    LONG arglen;

    seg = miniamp3_radio_worker_seglist;
    args = miniamp3_radio_worker_args;
    arglen = miniamp3_radio_worker_arglen;

    if (seg) {
        RunCommand(seg, 262144, (STRPTR)args, arglen);
        UnLoadSeg(seg);
    }
}

static struct Process *MiniAmp3CreateNewProcTagsWrapped(ULONG firstTag, ...)
{
    struct TagItem tags[MINIAMP3_PROC_WRAP_MAX_TAGS];
    struct TagItem launchTags[16];
    va_list ap;
    ULONG tag;
    ULONG data;
    int n;
    BPTR seglist;
    const char *args;
    const char *name;
    ULONG priority;
    ULONG stackSize;
    ULONG currentDir;
    ULONG output;
    ULONG closeOutput;
    ULONG copyVars;
    int isRadioWorker;

    n = 0;
    seglist = (BPTR)0;
    args = NULL;
    name = NULL;
    priority = 0;
    stackSize = 262144;
    currentDir = 0;
    output = 0;
    closeOutput = FALSE;
    copyVars = FALSE;

    va_start(ap, firstTag);
    tag = firstTag;
    while (tag != TAG_DONE && n + 1 < MINIAMP3_PROC_WRAP_MAX_TAGS) {
        data = va_arg(ap, ULONG);
        tags[n].ti_Tag = tag;
        tags[n].ti_Data = data;
        n++;

        if (tag == NP_Seglist)
            seglist = (BPTR)data;
        else if (tag == NP_Arguments)
            args = (const char *)data;
        else if (tag == NP_Name)
            name = (const char *)data;
        else if (tag == NP_Priority)
            priority = data;
        else if (tag == NP_StackSize)
            stackSize = data;
        else if (tag == NP_CurrentDir)
            currentDir = data;
        else if (tag == NP_Output)
            output = data;
        else if (tag == NP_CloseOutput)
            closeOutput = data;
        else if (tag == NP_CopyVars)
            copyVars = data;

        tag = va_arg(ap, ULONG);
    }
    va_end(ap);
    tags[n].ti_Tag = TAG_DONE;
    tags[n].ti_Data = 0;

    isRadioWorker = seglist && args && name && strstr(name, " radio ") != NULL;
    if (!isRadioWorker)
        return (struct Process *)CreateNewProc(tags);

    miniamp3_radio_worker_seglist = seglist;
    strncpy(miniamp3_radio_worker_args, args, sizeof(miniamp3_radio_worker_args) - 1);
    miniamp3_radio_worker_args[sizeof(miniamp3_radio_worker_args) - 1] = '\0';
    miniamp3_radio_worker_arglen = (LONG)strlen(miniamp3_radio_worker_args);

    n = 0;
    launchTags[n].ti_Tag = NP_Entry;       launchTags[n++].ti_Data = (ULONG)MiniAmp3RadioWorkerLauncherEntry;
    launchTags[n].ti_Tag = NP_Name;        launchTags[n++].ti_Data = (ULONG)name;
    launchTags[n].ti_Tag = NP_Priority;    launchTags[n++].ti_Data = priority;
    launchTags[n].ti_Tag = NP_StackSize;   launchTags[n++].ti_Data = stackSize;
    if (currentDir) {
        launchTags[n].ti_Tag = NP_CurrentDir;
        launchTags[n++].ti_Data = currentDir;
    }
    if (output) {
        launchTags[n].ti_Tag = NP_Output;
        launchTags[n++].ti_Data = output;
        launchTags[n].ti_Tag = NP_CloseOutput;
        launchTags[n++].ti_Data = closeOutput;
    }
    launchTags[n].ti_Tag = NP_CopyVars;    launchTags[n++].ti_Data = copyVars;
    launchTags[n].ti_Tag = TAG_DONE;       launchTags[n++].ti_Data = 0;

    return (struct Process *)CreateNewProc(launchTags);
}

#ifdef CreateNewProcTags
#undef CreateNewProcTags
#endif
#define CreateNewProcTags(...) MiniAmp3CreateNewProcTagsWrapped(__VA_ARGS__)
#endif /* AMIGA_M68K && !__ASSEMBLER__ */

#endif /* RADIO_WORKER_PROC_WRAP_H */
