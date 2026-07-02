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
#define RADIO_DBG(...) do { } while (0);
#define RADIO_DBG_PRINTF(x) do { } while (0);
#endif

#endif /* RADIO_DEBUG_H */
