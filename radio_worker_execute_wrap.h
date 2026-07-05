#ifndef RADIO_WORKER_EXECUTE_WRAP_H
#define RADIO_WORKER_EXECUTE_WRAP_H

/* Temporary ownership-safe radio-worker launch shim.
 *
 * PR #455 moved radio playback into amiga_mp3dec.fastexp so the worker has its
 * own data segment and owns bsdsocket.library/AmiSSL.  The GUI frontends still
 * launch it via Execute("Run >NIL: amiga_mp3dec.fastexp ..."), then try to find
 * a task named amiga_mp3dec.fastexp later.  On real AmigaOS that shell/RUN path
 * can leave Stop with no reliable task to signal.
 *
 * This wrapper keeps the source-level Execute() call sites intact for now, but
 * intercepts that exact radio-worker command and starts the worker with
 * CreateNewProcTags(NP_Command/NP_Arguments/NP_Name).  The worker therefore has
 * a real task name that the existing Stop fallback can find and signal with
 * SIGBREAKF_CTRL_C.
 *
 * Longer term, the frontends should call CreateNewProcTags() directly with a
 * per-run unique NP_Name.  This shim is deliberately narrow: non-radio Execute()
 * calls fall through to dos.library Execute().
 */

#if defined(AMIGA_M68K)
#include <string.h>
#include <exec/types.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>

static LONG MiniAmp3ExecuteTrackedRadioWorker(STRPTR command, BPTR input, BPTR output)
{
    const char *prefix;
    const char *cmd;
    const char *args;
    struct Process *thisProc;
    struct Process *worker;
    BPTR dirLock;
    BPTR workerOut;

    prefix = "Run >NIL: amiga_mp3dec.fastexp";
    cmd = (const char *)command;
    if (!cmd || strncmp(cmd, prefix, strlen(prefix)) != 0)
        return Execute(command, input, output);

    args = cmd + strlen(prefix);
    while (*args == ' ' || *args == '\t')
        args++;

    thisProc = (struct Process *)FindTask(NULL);
    dirLock = DupLock(thisProc ? thisProc->pr_CurrentDir : (BPTR)0);
    workerOut = Open((STRPTR)"NIL:", MODE_NEWFILE);
    if (!workerOut) {
        if (dirLock)
            UnLock(dirLock);
        return 0;
    }

    worker = CreateNewProcTags(
        NP_Command,     (ULONG)"amiga_mp3dec.fastexp",
        NP_Arguments,   (ULONG)args,
        NP_Name,        (ULONG)"amiga_mp3dec.fastexp",
        NP_Priority,    0,
        NP_StackSize,   262144,
        NP_CurrentDir,  dirLock,
        NP_Output,      workerOut,
        NP_CloseOutput, TRUE,
        NP_CopyVars,    FALSE,
        TAG_DONE);

    if (!worker) {
        Close(workerOut);
        if (dirLock)
            UnLock(dirLock);
        return 0;
    }

    return 1;
}

#define Execute(command, input, output) MiniAmp3ExecuteTrackedRadioWorker((command), (input), (output))
#endif /* AMIGA_M68K */

#endif /* RADIO_WORKER_EXECUTE_WRAP_H */
