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

#ifdef __cplusplus
}
#endif

#endif
