#ifndef RADIO_DEBUG_H
#define RADIO_DEBUG_H

#ifdef RADIO_DEBUG
/* fflush() after every debug statement: stdout is fully buffered when
 * redirected to a log file, so without this a hard lock or Guru can leave
 * the last several KB of already-executed debug output sitting unwritten,
 * making the log's last line look earlier than where execution actually
 * got to.  RADIO_DEBUG builds are for chasing exactly these crashes, so the
 * extra I/O cost here is worth trustworthy logs; release builds (RADIO_DEBUG
 * undefined) never see this. */
#include <stdio.h>

#if defined(AMIGA_M68K)
#include <exec/semaphores.h>
#include <proto/exec.h>
/* The net worker task, every playback child, and the GUI task all call these
 * macros concurrently and write to the same shared stdout. Captured field
 * logs show this directly: two tasks' printf() output physically spliced
 * together mid-line (e.g. "...ssl=0x4097ab24debug-cleanup:  ctx=0x...
 * request index=1 channel=0 AbortIO issued=1"), proving genuinely
 * unsynchronized concurrent writes to the C library's shared stdio buffer
 * state -- exactly the kind of race that can corrupt the buffer's own
 * bookkeeping and, by extension, nearby exec heap memory. This lines up with
 * every captured heap-corruption/AN_BadFreeAddr crash so far: they only ever
 * happen during Stop/close, exactly when multiple tasks are logging heavily
 * at the same time. Serialize all debug output through one semaphore,
 * initialized once at true program startup (see minimp3r.c's real main()) --
 * never re-initialized per playback child, which would race an in-progress
 * Obtain/Release from another task. */
extern struct SignalSemaphore radio_console_lock;
#define RADIO_DBG(...) do { ObtainSemaphore(&radio_console_lock); __VA_ARGS__; fflush(stdout); ReleaseSemaphore(&radio_console_lock); } while (0);
#define RADIO_DBG_PRINTF(x) do { ObtainSemaphore(&radio_console_lock); printf x; fflush(stdout); ReleaseSemaphore(&radio_console_lock); } while (0);
#else
#define RADIO_DBG(...) do { __VA_ARGS__; fflush(stdout); } while (0);
#define RADIO_DBG_PRINTF(x) do { printf x; fflush(stdout); } while (0);
#endif

#else
#define RADIO_DBG(...) do { } while (0);
#define RADIO_DBG_PRINTF(x) do { } while (0);
#endif

#endif /* RADIO_DEBUG_H */
