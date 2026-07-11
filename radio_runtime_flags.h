#ifndef RADIO_RUNTIME_FLAGS_H
#define RADIO_RUNTIME_FLAGS_H

#ifdef __cplusplus
extern "C" {
#endif

const char *radio_runtime_flag_raw_getenv(const char *name);
const char *radio_runtime_flag_raw_getvar(const char *name);
int radio_runtime_flag_enabled(const char *name);
void Radio_LogRuntimeFlagsOnce(void);
void Radio_LogTestModeSummary(void);

/* The single authoritative abort-time SSL/SSL_CTX free policy, used both by
 * the runtime-flag summary (Radio_LogTestModeSummary()) and by the actual
 * cleanup decision (radio_stream.c's radio_skip_abort_ssl_free()).  The
 * default is NOT a blanket skip: which close is which is decided per session:
 *   - clean completed connection ........ SSL_shutdown + SSL_free/SSL_CTX_free
 *   - user stop / station switch ........ SSL_free/SSL_CTX_free (no fault, safe)
 *   - handshake/read/write fault ........ quarantine (leak SSL/SSL_CTX)
 *   - detected memory/TLS poison ........ quarantine (leak, and no AmiSSL calls)
 * The two environment flags force one behaviour for every abort regardless of
 * session state (diagnostic isolation only). */
typedef enum {
    RADIO_ABORT_SSL_POLICY_FREE_UNLESS_FAULT = 0, /* default: free on clean abort, leak only on fault/poison */
    RADIO_ABORT_SSL_POLICY_FORCE_FREE,            /* MP3_ALLOW_ABORT_SSL_FREE: free even after a session fault */
    RADIO_ABORT_SSL_POLICY_FORCE_SKIP             /* MP3_SKIP_ABORT_SSL_FREE: leak on every abort */
} RadioAbortSslPolicy;
RadioAbortSslPolicy Radio_AbortSslFreePolicy(void);
const char *Radio_AbortSslFreePolicyName(void);

#ifdef __cplusplus
}
#endif

#endif
