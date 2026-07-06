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
#define RADIO_DBG(...) do { __VA_ARGS__; fflush(stdout); } while (0);
#define RADIO_DBG_PRINTF(x) do { printf x; fflush(stdout); } while (0);
#else
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* RC1 release builds still need critical user-facing/fault output, but a few
 * temporary pump/read diagnostics were deliberately plain printf() while we
 * were chasing the worker/AmiSSL crashes.  Keep those visible in RADIO_DEBUG
 * builds, and suppress only the known noisy steady-state lines here so RC1
 * does not spam the Shell during normal playback. */
static int radio_release_printf(const char *fmt, ...)
{
    int r;
    va_list ap;

    if (fmt &&
        (!strncmp(fmt, "radio-read: transient zero", 26) ||
         !strncmp(fmt, "radio-input: zero read", 22) ||
         !strncmp(fmt, "radio-worker: session=", 22) ||
         !strncmp(fmt, "radio-worker: backpressure", 26) ||
         !strncmp(fmt, "radio-pump: stop/detach observed", 33)))
        return 0;

    va_start(ap, fmt);
    r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}

#define printf radio_release_printf
#define RADIO_DBG(...) do { } while (0);
#define RADIO_DBG_PRINTF(x) do { } while (0);
#endif

#endif /* RADIO_DEBUG_H */
